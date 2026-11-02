// Peer sessions (CR024): what the rows mean is delivery's (an epoch's key, verification, a key bundle), over any
// store; the in-memory store keeps a direct thread and its account-level session on one row, as the app's does.
#include "lib/delivery/IPeerSessionStore.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "foundation/crypto/CryptoUtil.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

PeerSessionKey AccountKey(CryptoChannel channel = CryptoChannel::E2e) {
  PeerSessionKey key;
  key.peer_identity_kind = "account";
  key.peer_identity_value = "bob";
  key.channel = channel;
  return key;
}

PeerSessionKey DeviceKey() {
  PeerSessionKey key = AccountKey(CryptoChannel::E2ePublic);
  key.peer_device = "QmBobPhone";
  return key;
}

/** A well-formed master PSK (32 bytes, base64), distinct per `fill`. */
std::string Psk(uint8_t fill) {
  return Base64Encode(ByteVector(32, fill));
}

PeerSessionRecord Session(const PeerSessionKey& key, uint32_t epoch, const std::string& master) {
  PeerSessionRecord record;
  record.key = key;
  record.session_epoch = epoch;
  record.master_psk_b64 = master;
  return record;
}

TEST(PeerSessionStoreTest, AnEpochsKeyIsTheCurrentOneOrARetiredOne) {
  InMemoryDeliveryStore store;
  PeerSessionRecord record = Session(AccountKey(), 3, "current");
  record.retired_psks.push_back({2, "older", 1000});
  ASSERT_TRUE(store.Save(record));

  EXPECT_EQ(store.ResolveMasterPskForEpoch(AccountKey(), 3).value(), std::optional<std::string>("current"));
  EXPECT_EQ(store.ResolveMasterPskForEpoch(AccountKey(), 2).value(), std::optional<std::string>("older"));
  EXPECT_EQ(store.ResolveMasterPskForEpoch(AccountKey(), 1).value(), std::nullopt) << "an epoch nobody kept";
  EXPECT_EQ(store.ResolveMasterPskForEpoch(DeviceKey(), 3).value(), std::nullopt) << "another session";
}

TEST(PeerSessionStoreTest, VerifyingNeedsTheSessionAndSticks) {
  InMemoryDeliveryStore store;
  EXPECT_FALSE(store.MarkPskVerified(AccountKey(), 5)) << "no session to verify";
  ASSERT_TRUE(store.Save(Session(AccountKey(), 1, "k")));
  EXPECT_FALSE(store.IsPskVerified(AccountKey()).value());
  ASSERT_TRUE(store.MarkPskVerified(AccountKey(), 5));
  EXPECT_TRUE(store.IsPskVerified(AccountKey()).value());
}

TEST(PeerSessionStoreTest, AnImportedBundleIsUnverifiedAndKeepsTheRetiredTail) {
  InMemoryDeliveryStore store;
  PeerSessionRecord old = Session(AccountKey(), 1, Psk(1));
  old.psk_verified_at = 5;
  ASSERT_TRUE(store.Save(old));
  ASSERT_TRUE(store.MarkPskVerified(AccountKey(), 5));

  PskBundleV1 bundle;
  bundle.channel = CryptoChannel::E2e;
  bundle.active_epoch = 2;
  bundle.master_psk_b64 = Psk(2);
  bundle.retired_epochs.push_back({1, Psk(1), 10});
  ASSERT_TRUE(store.ImportPskBundle(AccountKey(), bundle));

  EXPECT_FALSE(store.IsPskVerified(AccountKey()).value()) << "a new key is verified again";
  EXPECT_EQ(store.ResolveMasterPskForEpoch(AccountKey(), 2).value(), std::optional<std::string>(Psk(2)));
  EXPECT_EQ(store.ResolveMasterPskForEpoch(AccountKey(), 1).value(), std::optional<std::string>(Psk(1)));
  auto exported = store.ExportPskBundle(AccountKey());
  ASSERT_TRUE(exported);
  EXPECT_EQ(exported->active_epoch, 2u);
  EXPECT_EQ(exported->master_psk_b64, Psk(2));

  bundle.channel = CryptoChannel::E2ePublic;
  EXPECT_FALSE(store.ImportPskBundle(AccountKey(), bundle)) << "a bundle for another channel";
}

TEST(PeerSessionStoreTest, ADirectThreadAndItsAccountSessionShareTheEpoch) {
  InMemoryDeliveryStore store;
  // The session first (a key arrives before the chat is opened), then the thread joins its row.
  ASSERT_TRUE(store.Save(Session(AccountKey(), 4, "k")));
  DirectChatTarget target;
  target.peer_identity_kind = "account";
  target.peer_identity_value = "bob";
  target.channel = ThreadChannel::E2e;
  auto thread = store.FindOrCreateDirectThread(target, "contact:bob", "Bob");
  ASSERT_TRUE(thread);
  EXPECT_EQ(store.GetChatTargetSessionEpoch(thread->id).value(), 4u);

  // And the other way: the thread's epoch moves, the session sees it.
  ASSERT_TRUE(store.BumpLocalChatTargetEpoch(thread->id));
  EXPECT_EQ(store.Load(AccountKey()).value()->session_epoch, 5u);
  EXPECT_EQ(store.Load(AccountKey()).value()->master_psk_b64, std::optional<std::string>("k"));
}

TEST(PeerSessionStoreTest, ADevicePairNumbersOnItsOwnAndRestartsInANewEpoch) {
  InMemoryDeliveryStore store;
  EXPECT_FALSE(store.AllocateSenderSeq(DeviceKey())) << "no session to number in";
  EXPECT_FALSE(store.AllocateSenderSeq(AccountKey())) << "the account's numbering is the thread's";
  ASSERT_TRUE(store.Save(Session(DeviceKey(), 1, "d")));
  EXPECT_EQ(store.AllocateSenderSeq(DeviceKey()).value(), 1u);
  EXPECT_EQ(store.AllocateSenderSeq(DeviceKey()).value(), 2u);
  ASSERT_TRUE(store.Save(Session(DeviceKey(), 1, "d")));
  EXPECT_EQ(store.AllocateSenderSeq(DeviceKey()).value(), 3u) << "a save in the same epoch keeps the numbering";
  ASSERT_TRUE(store.Save(Session(DeviceKey(), 2, "d2")));
  EXPECT_EQ(store.AllocateSenderSeq(DeviceKey()).value(), 1u);
}

TEST(PeerSessionStoreTest, ListHasTheSessionsWithKeysOnly) {
  InMemoryDeliveryStore store;
  DirectChatTarget target;
  target.peer_identity_kind = "account";
  target.peer_identity_value = "carol";
  ASSERT_TRUE(store.FindOrCreateDirectThread(target, "contact:carol", "Carol"));
  ASSERT_TRUE(store.Save(Session(AccountKey(), 1, "k")));
  ASSERT_TRUE(store.Save(Session(DeviceKey(), 1, "d")));
  EXPECT_EQ(store.List().value().size(), 2u) << "Carol's thread has no session key";
}

TEST(ControlLaneSeenStoreTest, AnIdIsSeenOnceUntilForgotten) {
  InMemoryControlLaneSeenStore seen;
  EXPECT_TRUE(seen.FirstSight("bob", "c1", 0).value());
  EXPECT_FALSE(seen.FirstSight("bob", "c1", 0).value());
  EXPECT_TRUE(seen.FirstSight("carol", "c1", 0).value()) << "per sender";
  seen.Forget("bob", "c1");
  EXPECT_TRUE(seen.FirstSight("bob", "c1", 0).value());
}

} // namespace
} // namespace pbr
