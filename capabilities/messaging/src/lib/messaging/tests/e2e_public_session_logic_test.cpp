#include "lib/delivery/E2ePublicSessionLogic.h"

#include "lib/delivery/AutoKeyEstablishment.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/HybridKem.h"
#include "foundation/crypto/CryptoUtil.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "common/Utilities.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <gtest/gtest.h>
#include <thread>

namespace {

using namespace pbr;

ByteVector TestDek() {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xb1 + i);
  }
  return dek;
}

class E2ePublicSessionLogicTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ =
        std::filesystem::temp_directory_path() / ("pp_browser_e2e_public_session_logic_" + util::GenerateUuid());
    std::filesystem::remove_all(data_dir_);
    store_ = std::make_unique<InMemoryDeliveryStore>();
    psk_ = std::make_unique<InMemoryPeerSessions>(*store_);
    ASSERT_TRUE(static_cast<bool>(store_->SetDek(TestDek())));
    ASSERT_TRUE(static_cast<bool>(psk_->SetDek(TestDek())));
    // Create profile.db (and chat_targets) before PSK OpenDb.
    ASSERT_TRUE(static_cast<bool>(store_->ListThreads()));
  }

  void TearDown() override {
    psk_.reset();
    store_.reset();
    std::filesystem::remove_all(data_dir_);
  }

  std::filesystem::path data_dir_;
  std::unique_ptr<InMemoryDeliveryStore> store_;
  std::unique_ptr<InMemoryPeerSessions> psk_;
};

// M021: a session per device pair, apart from the account's and from the peer's other devices, numbered in
// the pair — a new epoch numbers from 1.
TEST_F(E2ePublicSessionLogicTest, EachPeerDeviceHasItsOwnSessionAndNumbering) {
  auto phone = HybridKem::GenerateKeyPair();
  auto laptop = HybridKem::GenerateKeyPair();
  ASSERT_TRUE(phone && laptop);
  const PeerSessionKey account{"account", "account:bob", CryptoChannel::E2ePublic};
  PeerSessionKey bob_phone = account;
  bob_phone.peer_device = "QmBobPhone";
  PeerSessionKey bob_laptop = account;
  bob_laptop.peer_device = "QmBobLaptop";

  auto with_phone = EnsureE2ePublicMasterPsk(*psk_, bob_phone, 1, phone->public_key);
  auto with_laptop = EnsureE2ePublicMasterPsk(*psk_, bob_laptop, 1, laptop->public_key);
  ASSERT_TRUE(with_phone && with_laptop);
  EXPECT_NE(with_phone->master_psk, with_laptop->master_psk);
  EXPECT_FALSE(psk_->Load(account).value().has_value()) << "no account-level session made";
  EXPECT_EQ(psk_->Load(bob_phone).value()->key, bob_phone);

  EXPECT_EQ(psk_->AllocateSenderSeq(bob_phone).value(), 1u);
  EXPECT_EQ(psk_->AllocateSenderSeq(bob_phone).value(), 2u);
  EXPECT_EQ(psk_->AllocateSenderSeq(bob_laptop).value(), 1u) << "each pair counts on its own";
  EXPECT_FALSE(psk_->AllocateSenderSeq(account)) << "the account's numbering is the thread's";
  PeerSessionKey nobody = account;
  nobody.peer_device = "QmUnknown";
  EXPECT_FALSE(psk_->AllocateSenderSeq(nobody)) << "no session to number in";

  // The pair restarts at its next epoch (D014): numbering there starts over; the laptop's goes on.
  ASSERT_TRUE(RestartE2ePublicSession(*psk_, bob_phone, 1, 2, 1000));
  ASSERT_TRUE(EnsureE2ePublicMasterPsk(*psk_, bob_phone, 2, phone->public_key));
  EXPECT_EQ(psk_->AllocateSenderSeq(bob_phone).value(), 1u);
  EXPECT_EQ(psk_->AllocateSenderSeq(bob_laptop).value(), 2u);
  EXPECT_TRUE(psk_->ResolveMasterPskForEpoch(bob_phone, 1).value().has_value()) << "the old epoch kept, retired";

  auto listed = psk_->List();
  ASSERT_TRUE(listed);
  EXPECT_EQ(std::count_if(listed->begin(), listed->end(),
                          [](const PeerSessionRecord& r) { return r.key.peer_identity_value == "account:bob"; }),
            2);
}

TEST_F(E2ePublicSessionLogicTest, CreatesOnceAndAnnouncesUntilConfirmed) {
  auto bob = HybridKem::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(bob));

  PeerSessionKey key;
  key.peer_identity_kind = "account";
  key.peer_identity_value = "account:bob";
  key.channel = CryptoChannel::E2ePublic;

  auto first = EnsureE2ePublicMasterPsk(*psk_, key, 1, bob->public_key);
  ASSERT_TRUE(static_cast<bool>(first)) << first.error().message;
  EXPECT_TRUE(first->created);
  ASSERT_TRUE(first->key_init_b64.has_value());
  EXPECT_FALSE(first->key_init_b64->empty());

  auto second = EnsureE2ePublicMasterPsk(*psk_, key, 1, bob->public_key);
  ASSERT_TRUE(static_cast<bool>(second));
  EXPECT_FALSE(second->created);
  EXPECT_EQ(second->key_init_b64, first->key_init_b64) << "not confirmed yet: the same key_init again";
  EXPECT_EQ(second->master_psk, first->master_psk);

  // Bob wrote under it: confirmed, no more key_init.
  ASSERT_TRUE(ReconcileE2ePublicSession(*psk_, key, 1, AutoKeyRead{first->master_psk, false}, std::nullopt, 1000));
  auto third = EnsureE2ePublicMasterPsk(*psk_, key, 1, bob->public_key);
  ASSERT_TRUE(static_cast<bool>(third));
  EXPECT_FALSE(third->key_init_b64.has_value());

  auto opened = AutoKeyEstablishment::DeriveMasterPskFromKeyInit(bob->private_key, *first->key_init_b64);
  ASSERT_TRUE(static_cast<bool>(opened));
  EXPECT_EQ(*opened, first->master_psk);
}

// Two sends to one device pair at once (a relay grant on one worker lane, a call invite on another) that both find no
// session: one opens it, and the other uses that one. Before, each opened its own and the later write dropped the
// earlier — whose key_init the peer then followed, and nothing it sent opened here again (the lab's lost call accept).
TEST_F(E2ePublicSessionLogicTest, SendsOpeningAPairAtOnceShareOneSession) {
  auto bob = HybridKem::GenerateKeyPair();
  ASSERT_TRUE(static_cast<bool>(bob));
  PeerSessionKey key{"account", "account:bob", CryptoChannel::E2ePublic};
  key.peer_device = "QmBobPhone";
  std::atomic<int> waiting{0};
  const auto slow_key = [&]() -> Roe<ByteVector> {
    ++waiting;
    while (waiting.load() < 2) { // both are past their "none yet" before either opens
      std::this_thread::yield();
    }
    return bob->public_key;
  };
  Roe<E2ePublicPskEnsureResult> first = Error("not run");
  Roe<E2ePublicPskEnsureResult> second = Error("not run");
  std::thread a([&] { first = EnsureE2ePublicMasterPsk(*psk_, key, 1, slow_key); });
  std::thread b([&] { second = EnsureE2ePublicMasterPsk(*psk_, key, 1, slow_key); });
  a.join();
  b.join();
  ASSERT_TRUE(first && second);
  EXPECT_EQ(first->master_psk, second->master_psk) << "one session";
  EXPECT_EQ(first->key_init_b64, second->key_init_b64) << "one opener, the stored one";
  EXPECT_NE(first->created, second->created);
  auto stored = psk_->ResolveMasterPskForEpoch(key, 1);
  ASSERT_TRUE(stored && stored->has_value());
  EXPECT_EQ(Base64Decode(**stored).value(), first->master_psk);
}

/** Both sides of a pair opening a session at the same time: each keeps or moves, and they agree. */
TEST_F(E2ePublicSessionLogicTest, BothOpeningAtOnceSettleOnTheLowerId) {
  auto alice = HybridKem::GenerateKeyPair();
  auto bob = HybridKem::GenerateKeyPair();
  ASSERT_TRUE(alice && bob);
  PeerSessionKey to_bob{"account", "account:bob", CryptoChannel::E2ePublic};
  PeerSessionKey to_alice{"account", "account:alice", CryptoChannel::E2ePublic};
  InMemoryPeerSessions bob_psk(*store_);
  ASSERT_TRUE(bob_psk.SetDek(TestDek()));

  auto alice_opened = EnsureE2ePublicMasterPsk(*psk_, to_bob, 1, bob->public_key);
  auto bob_opened = EnsureE2ePublicMasterPsk(bob_psk, to_alice, 1, alice->public_key);
  ASSERT_TRUE(alice_opened && bob_opened);
  const std::string alice_id = AutoKeySessionId(*alice_opened->key_init_b64);
  const std::string bob_id = AutoKeySessionId(*bob_opened->key_init_b64);

  // Each reads the other's first send under the key_init it carries.
  auto at_alice = AutoKeyEstablishment::DeriveMasterPskFromKeyInit(alice->private_key, *bob_opened->key_init_b64);
  auto at_bob = AutoKeyEstablishment::DeriveMasterPskFromKeyInit(bob->private_key, *alice_opened->key_init_b64);
  ASSERT_TRUE(at_alice && at_bob);
  ASSERT_TRUE(
      ReconcileE2ePublicSession(*psk_, to_bob, 1, AutoKeyRead{*at_alice, true}, bob_opened->key_init_b64, 1000));
  ASSERT_TRUE(
      ReconcileE2ePublicSession(bob_psk, to_alice, 1, AutoKeyRead{*at_bob, true}, alice_opened->key_init_b64, 1000));

  auto alice_now = psk_->Load(to_bob);
  auto bob_now = bob_psk.Load(to_alice);
  ASSERT_TRUE(alice_now && alice_now->has_value() && bob_now && bob_now->has_value());
  EXPECT_EQ((*alice_now)->master_psk_b64, (*bob_now)->master_psk_b64) << "one session for the pair";
  EXPECT_EQ((*alice_now)->auto_key_session_id, std::optional<std::string>(std::min(alice_id, bob_id)));
}

TEST_F(E2ePublicSessionLogicTest, AConfirmedSessionGivesWayToTheOneThePeerNowUses) {
  auto bob = HybridKem::GenerateKeyPair();
  ASSERT_TRUE(bob);
  PeerSessionKey key{"account", "account:bob", CryptoChannel::E2ePublic};
  auto opened = EnsureE2ePublicMasterPsk(*psk_, key, 1, bob->public_key);
  ASSERT_TRUE(opened);
  ASSERT_TRUE(ReconcileE2ePublicSession(*psk_, key, 1, AutoKeyRead{opened->master_psk, false}, std::nullopt, 1000));

  // Bob starts over (reinstall) and opens a new session to us.
  auto alice = HybridKem::GenerateKeyPair();
  ASSERT_TRUE(alice);
  auto fresh = AutoKeyEstablishment::EncapsulateForRecipient(alice->public_key);
  ASSERT_TRUE(fresh);
  ASSERT_TRUE(
      ReconcileE2ePublicSession(*psk_, key, 1, AutoKeyRead{fresh->master_psk, true}, fresh->key_init_b64, 2000));
  auto now = psk_->Load(key);
  ASSERT_TRUE(now && now->has_value());
  EXPECT_EQ((*now)->master_psk_b64, std::optional<std::string>(Base64Encode(fresh->master_psk)));
  EXPECT_FALSE((*now)->auto_key_init_pending.has_value());
}

/** The pair settled on one session and the peer has written under it since: an opener for another
 *  session signed before that (it arrived late) does not move the pair back; a later one does. */
TEST_F(E2ePublicSessionLogicTest, AnOlderOpenerDoesNotUndoTheSessionThePeerUses) {
  auto alice = HybridKem::GenerateKeyPair();
  ASSERT_TRUE(alice);
  PeerSessionKey key{"account", "account:bob", CryptoChannel::E2ePublic};
  auto current = AutoKeyEstablishment::EncapsulateForRecipient(alice->public_key);
  auto earlier = AutoKeyEstablishment::EncapsulateForRecipient(alice->public_key);
  ASSERT_TRUE(current && earlier);
  // Bob's session, then Bob writing under it at t=3000.
  ASSERT_TRUE(
      ReconcileE2ePublicSession(*psk_, key, 1, AutoKeyRead{current->master_psk, true}, current->key_init_b64, 2000));
  ASSERT_TRUE(ReconcileE2ePublicSession(*psk_, key, 1, AutoKeyRead{current->master_psk, false}, std::nullopt, 3000));

  ASSERT_TRUE(
      ReconcileE2ePublicSession(*psk_, key, 1, AutoKeyRead{earlier->master_psk, true}, earlier->key_init_b64, 1500));
  auto now = psk_->Load(key);
  ASSERT_TRUE(now && now->has_value());
  EXPECT_EQ((*now)->master_psk_b64, std::optional<std::string>(Base64Encode(current->master_psk))) << "kept";

  auto later = AutoKeyEstablishment::EncapsulateForRecipient(alice->public_key);
  ASSERT_TRUE(later);
  ASSERT_TRUE(
      ReconcileE2ePublicSession(*psk_, key, 1, AutoKeyRead{later->master_psk, true}, later->key_init_b64, 4000));
  now = psk_->Load(key);
  ASSERT_TRUE(now && now->has_value());
  EXPECT_EQ((*now)->master_psk_b64, std::optional<std::string>(Base64Encode(later->master_psk)))
      << "a newer start wins";
}

} // namespace
