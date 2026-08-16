#include "base/crypto/AutoKeyEstablishment.h"
#include "base/crypto/CryptoConstants.h"
#include "base/crypto/CryptoUtil.h"
#include "base/crypto/HybridKem.h"
#include "base/messaging/DirectChatTarget.h"
#include "base/messaging/PskRotateCodec.h"
#include "base/messaging/RelayEnvelope.h"
#include "base/messaging/SqliteThreadStore.h"
#include "base/messaging/ThreadTypes.h"
#include "feature/messaging/LinkDeviceCoordinator.h"
#include "feature/messaging/PublicPskLockCoordinator.h"
#include "feature/messaging/SqlitePskSessionStore.h"

#include <filesystem>
#include <gtest/gtest.h>

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xa0 + i);
  }
  return dek;
}

ByteVector TestMasterPsk() {
  ByteVector psk(kMasterPskSize);
  for (size_t i = 0; i < psk.size(); ++i) {
    psk[i] = static_cast<uint8_t>(i + 1);
  }
  return psk;
}

std::filesystem::path MakeLockDir(const std::string& suffix) {
  auto dir = std::filesystem::temp_directory_path() / ("pp_public_lock_" + suffix);
  std::filesystem::remove_all(dir);
  return dir;
}

struct LockFixture {
  std::filesystem::path dir;
  SqliteThreadStore store;
  SqlitePskSessionStore psk_store;
  PublicPskLockCoordinator lock;
  std::string thread_id;
  ChatTargetKey key;

  explicit LockFixture(const std::string& suffix)
      : dir(MakeLockDir(suffix)),
        store(dir.string()),
        psk_store(store.ProfileDbPath(), suffix),
        lock(store, psk_store) {
    EXPECT_TRUE(static_cast<bool>(psk_store.SetDek(TestDek())));
    DirectChatTarget target;
    target.peer_identity_kind = "account";
    target.peer_identity_value = "account:bob";
    target.channel = ThreadChannel::E2ePublic;
    auto thread = store.FindOrCreateDirectThread(target, "contact-bob", "Bob");
    EXPECT_TRUE(static_cast<bool>(thread));
    thread_id = thread->id;
    key.peer_identity_kind = "account";
    key.peer_identity_value = "account:bob";
    key.channel = CryptoChannel::E2ePublic;
    PskSessionRecord record;
    record.key = key;
    record.session_epoch = 1;
    record.master_psk_b64 = Base64Encode(TestMasterPsk());
    record.key_scope = PublicKeyScope::Account;
    EXPECT_TRUE(static_cast<bool>(psk_store.Save(record)));
  }

  ~LockFixture() { std::filesystem::remove_all(dir); }
};

ThreadMessage RotateMessage(const PskRotateDetail& detail) {
  ThreadMessage message;
  message.content_type = ChatContentType::System;
  auto json = PskRotateCodec::EncodePayloadJson(detail);
  EXPECT_TRUE(static_cast<bool>(json)) << json.error().message;
  message.payload_json = *json;
  message.text = "New messages stay on this device.";
  return message;
}

RelayEnvelope RotateEnvelope(const std::string& key_init_b64, const uint32_t epoch) {
  RelayEnvelope envelope;
  envelope.envelope_version = kRelayEnvelopeVersion;
  envelope.session_epoch = epoch;
  envelope.body.e2e.key_init_b64 = key_init_b64;
  return envelope;
}

TEST(PublicPskLockCoordinatorTest, HashMismatchRejects) {
  LockFixture alice("hash");
  auto bob_kem = HybridKem::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(bob_kem));
  auto plan = alice.lock.PrepareLock(alice.thread_id, bob_kem->public_key, 1000);
  ASSERT_TRUE(static_cast<bool>(plan)) << plan.error().message;
  plan->detail.key_init_hash = std::string(64, '0');
  auto message = RotateMessage(plan->detail);
  auto envelope = RotateEnvelope(plan->key_init_b64, plan->old_epoch);
  auto applied = alice.lock.ApplyInbound(alice.thread_id, envelope, message, bob_kem->private_key, "account:bob", 1001);
  ASSERT_FALSE(static_cast<bool>(applied));
  EXPECT_NE(applied.error().message.find("key_init_hash"), std::string::npos);
}

TEST(PublicPskLockCoordinatorTest, SiblingOfLockerLocksOut) {
  LockFixture alice("sibling");
  auto bob_kem = HybridKem::GenerateKeyPair();
  auto alice_kem = HybridKem::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(bob_kem));
  ASSERT_TRUE(static_cast<bool>(alice_kem));
  auto plan = alice.lock.PrepareLock(alice.thread_id, bob_kem->public_key, 1000);
  ASSERT_TRUE(static_cast<bool>(plan)) << plan.error().message;
  ASSERT_TRUE(static_cast<bool>(alice.lock.AbortPrepare(*plan)));

  PskSessionRecord sibling = alice.psk_store.Load(alice.key)->value();
  sibling.last_rotation_id.reset();
  sibling.thread_kem_pk_b64.reset();
  sibling.thread_kem_sk_b64.reset();
  ASSERT_TRUE(static_cast<bool>(alice.psk_store.Save(sibling)));

  auto message = RotateMessage(plan->detail);
  auto envelope = RotateEnvelope(plan->key_init_b64, plan->old_epoch);
  envelope.sender_contact_id = "account:alice";
  auto applied =
      alice.lock.ApplyInbound(alice.thread_id, envelope, message, alice_kem->private_key, "account:alice", 1001);
  ASSERT_TRUE(static_cast<bool>(applied)) << applied.error().message;
  EXPECT_EQ(*applied, PublicKeyScope::LockedOut);
  auto loaded = alice.psk_store.Load(alice.key);
  ASSERT_TRUE(static_cast<bool>(loaded));
  ASSERT_TRUE(loaded->has_value());
  EXPECT_EQ(loaded->value().key_scope, PublicKeyScope::LockedOut);
  EXPECT_EQ(*loaded->value().master_psk_b64, Base64Encode(TestMasterPsk()));
}

TEST(PublicPskLockCoordinatorTest, PeerAdoptsAndInitiatorLocks) {
  LockFixture alice("alice");
  LockFixture bob("bob");
  bob.key.peer_identity_value = "account:alice";
  DirectChatTarget bob_target;
  bob_target.peer_identity_kind = "account";
  bob_target.peer_identity_value = "account:alice";
  bob_target.channel = ThreadChannel::E2ePublic;
  auto bob_thread = bob.store.FindOrCreateDirectThread(bob_target, "contact-alice", "Alice");
  ASSERT_TRUE(static_cast<bool>(bob_thread));
  bob.thread_id = bob_thread->id;
  PskSessionRecord bob_record;
  bob_record.key = bob.key;
  bob_record.session_epoch = 1;
  bob_record.master_psk_b64 = Base64Encode(TestMasterPsk());
  ASSERT_TRUE(static_cast<bool>(bob.psk_store.Save(bob_record)));
  PublicPskLockCoordinator bob_lock(bob.store, bob.psk_store);

  auto bob_kem = HybridKem::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(bob_kem));
  auto plan = alice.lock.PrepareLock(alice.thread_id, bob_kem->public_key, 2000);
  ASSERT_TRUE(static_cast<bool>(plan)) << plan.error().message;
  EXPECT_EQ(plan->detail.wrap_kind, kPskRotateWrapAccountKem);
  EXPECT_EQ(plan->next_scope, PublicKeyScope::DeviceSelf);

  auto message = RotateMessage(plan->detail);
  auto envelope = RotateEnvelope(plan->key_init_b64, plan->old_epoch);
  envelope.sender_contact_id = "account:alice";
  auto bob_scope =
      bob_lock.ApplyInbound(bob.thread_id, envelope, message, bob_kem->private_key, "account:bob", 2001);
  ASSERT_TRUE(static_cast<bool>(bob_scope)) << bob_scope.error().message;
  EXPECT_EQ(*bob_scope, PublicKeyScope::Account);

  ASSERT_TRUE(static_cast<bool>(alice.lock.Commit(*plan, 2002)));
  auto alice_scope = alice.lock.GetKeyScope(alice.thread_id);
  ASSERT_TRUE(static_cast<bool>(alice_scope));
  EXPECT_EQ(*alice_scope, PublicKeyScope::DeviceSelf);

  auto bob_loaded = bob.psk_store.Load(bob.key);
  ASSERT_TRUE(static_cast<bool>(bob_loaded) && bob_loaded->has_value());
  EXPECT_EQ(bob_loaded->value().session_epoch, 2u);
  EXPECT_NE(*bob_loaded->value().master_psk_b64, Base64Encode(TestMasterPsk()));
  EXPECT_TRUE(bob_loaded->value().peer_thread_kem_pk_b64.has_value());
}

TEST(PublicPskLockCoordinatorTest, SecondLockUsesConversationKem) {
  LockFixture alice("pair_a");
  auto bob_kem = HybridKem::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(bob_kem));
  auto first = alice.lock.PrepareLock(alice.thread_id, bob_kem->public_key, 3000);
  ASSERT_TRUE(static_cast<bool>(first));
  ASSERT_TRUE(static_cast<bool>(alice.lock.Commit(*first, 3001)));

  PskSessionRecord record = alice.psk_store.Load(alice.key)->value();
  record.key_scope = PublicKeyScope::Account;
  record.peer_thread_kem_pk_b64 = record.thread_kem_pk_b64;
  ASSERT_TRUE(static_cast<bool>(alice.psk_store.Save(record)));

  auto second = alice.lock.PrepareLock(alice.thread_id, bob_kem->public_key, 3002);
  ASSERT_TRUE(static_cast<bool>(second)) << second.error().message;
  EXPECT_EQ(second->detail.wrap_kind, kPskRotateWrapThreadKem);
  EXPECT_EQ(second->next_scope, PublicKeyScope::DevicePair);
}

TEST(PublicPskLockCoordinatorTest, ConcurrentInboundWinnerAbortsLocalCommit) {
  LockFixture alice("race");
  auto bob_kem = HybridKem::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(bob_kem));
  auto local = alice.lock.PrepareLock(alice.thread_id, bob_kem->public_key, 4000);
  ASSERT_TRUE(static_cast<bool>(local));

  PskRotateDetail inbound = local->detail;
  inbound.rotation_id = local->detail.rotation_id;
  inbound.rotation_id.back() = inbound.rotation_id.back() == 'z' ? 'y' : 'z';
  if (!PskRotateCodec::RotationIdWins(inbound.rotation_id, local->detail.rotation_id)) {
    inbound.rotation_id = "zzzzzzzz-zzzz-zzzz-zzzz-zzzzzzzzzzzz";
  }
  auto established = AutoKeyEstablishment::EncapsulateForRecipient(bob_kem->public_key);
  ASSERT_TRUE(static_cast<bool>(established));
  auto hash = AutoKeyEstablishment::HashKeyInitB64(established->key_init_b64);
  ASSERT_TRUE(static_cast<bool>(hash));
  inbound.key_init_hash = *hash;
  inbound.thread_kem_pk_b64 = local->detail.thread_kem_pk_b64;
  inbound.wrap_kind = kPskRotateWrapAccountKem;
  inbound.new_epoch = 2;

  auto message = RotateMessage(inbound);
  auto envelope = RotateEnvelope(established->key_init_b64, 1);
  envelope.sender_contact_id = "account:bob";
  auto applied =
      alice.lock.ApplyInbound(alice.thread_id, envelope, message, bob_kem->private_key, "account:alice", 4001);
  ASSERT_TRUE(static_cast<bool>(applied)) << applied.error().message;

  ASSERT_TRUE(static_cast<bool>(alice.lock.Commit(*local, 4002)));
  auto loaded = alice.psk_store.Load(alice.key);
  ASSERT_TRUE(static_cast<bool>(loaded) && loaded->has_value());
  EXPECT_EQ(*loaded->value().last_rotation_id, inbound.rotation_id);
  EXPECT_EQ(loaded->value().session_epoch, 2u);
}

TEST(PublicPskLockCoordinatorTest, AutoRekeyOnlyWhenBothDeviceBound) {
  LockFixture alice("auto");
  EXPECT_FALSE(*alice.lock.ShouldAutoRekey(alice.thread_id, 10));

  auto loaded = alice.psk_store.Load(alice.key);
  PskSessionRecord record = loaded->value();
  record.key_scope = PublicKeyScope::DevicePair;
  record.peer_thread_kem_pk_b64 = "dGVzdA==";
  record.psk_rotate_msg_count = kPublicPskAutoRotateMsgCount;
  ASSERT_TRUE(static_cast<bool>(alice.psk_store.Save(record)));
  EXPECT_TRUE(*alice.lock.ShouldAutoRekey(alice.thread_id, 10));

  record.psk_rotate_msg_count = 0;
  record.last_psk_rotate_at = 1;
  ASSERT_TRUE(static_cast<bool>(alice.psk_store.Save(record)));
  EXPECT_TRUE(*alice.lock.ShouldAutoRekey(alice.thread_id, 1 + kPublicPskAutoRotateIntervalMs));

  record.key_scope = PublicKeyScope::DeviceSelf;
  ASSERT_TRUE(static_cast<bool>(alice.psk_store.Save(record)));
  EXPECT_FALSE(*alice.lock.ShouldAutoRekey(alice.thread_id, 1 + kPublicPskAutoRotateIntervalMs));
}

TEST(PublicPskLockCoordinatorTest, LinkExportOmitsDeviceScopedPsk) {
  LockFixture alice("link");
  auto loaded = alice.psk_store.Load(alice.key);
  PskSessionRecord record = loaded->value();
  record.key_scope = PublicKeyScope::DeviceSelf;
  ASSERT_TRUE(static_cast<bool>(alice.psk_store.Save(record)));
  auto collected = LinkDeviceCoordinator::CollectPublicPsks(alice.psk_store);
  ASSERT_TRUE(static_cast<bool>(collected));
  EXPECT_TRUE(collected->empty());
}

} // namespace
} // namespace pbr
