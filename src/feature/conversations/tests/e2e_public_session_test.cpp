// The pair's e2e_public session as two real sides use it: each side opens it on its first send
// (AutoKey to the peer's KEM key) and the peer derives it from that send's key_init. These are the
// ways real calls stop getting through: both sides open at once, the opening send is lost, one side
// starts over (reinstall / wiped store).

#include "common/chat/SystemControl.h"
#include "domain/messaging/ControlLaneEnvelope.h"
#include "domain/messaging/ControlLaneSeenStore.h"
#include "domain/messaging/E2ePublicSessionLogic.h"
#include "domain/messaging/GroupRosterStore.h"
#include "domain/messaging/PeerSigningKeyStore.h"
#include "domain/messaging/SqlitePskSessionStore.h"
#include "domain/messaging/SqliteThreadStore.h"
#include "domain/people/IdentityStore.h"
#include "domain/messaging/RelayReceivePipeline.h"
#include "feature/conversations/LocalKeysFromIdentity.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "common/directory/DirectoryJson.h"
#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {
namespace {

ByteVector TestDek(uint8_t seed) {
  ByteVector dek(kDataEncryptionKeySize);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(seed + i);
  }
  return dek;
}

/** One device: its identity, stores and receive pipeline; a lane family that counts what lands. */
struct Side {
  explicit Side(const std::string& tag, uint8_t seed) : tag(tag), seed(seed) {
    root = std::filesystem::temp_directory_path() / ("pp_session_" + tag + "_" + util::GenerateUuid());
    std::filesystem::create_directories(root);
    identity = std::make_unique<IdentityStore>((root / "identity").string(), tag);
    std::filesystem::create_directories(root / "identity");
    if (!identity->SetDek(TestDek(seed)) || !identity->LoadOrCreate()) {
      throw std::runtime_error("identity");
    }
    account = identity->Get()->account_id;
    OpenProfile("profile-1");
  }

  ~Side() {
    pipeline.reset();
    seen.reset();
    psk.reset();
    roster.reset();
    store.reset();
    identity.reset();
    std::filesystem::remove_all(root);
  }

  /** (Re)open the profile's message and session stores: a reinstall starts from an empty one. */
  void OpenProfile(const std::string& dir) {
    pipeline.reset();
    seen.reset();
    psk.reset();
    roster.reset();
    store.reset();
    const auto data_dir = root / dir;
    store = std::make_unique<SqliteThreadStore>(data_dir.string());
    if (!store->SetDek(TestDek(seed)) || !store->ListThreads()) {
      throw std::runtime_error("store");
    }
    roster = std::make_unique<GroupRosterStore>(store->ProfileDbPath());
    psk = std::make_unique<SqlitePskSessionStore>(store->ProfileDbPath(), tag);
    if (!psk->SetDek(TestDek(seed))) {
      throw std::runtime_error("psk");
    }
    seen = std::make_unique<ControlLaneSeenStore>(store->ProfileDbPath());
    pipeline = std::make_unique<RelayReceivePipeline>(*store, resolver, *psk, LocalKeysFromIdentity(*identity), *roster);
    pipeline->SetSystemControls(&controls);
    pipeline->SetControlLaneSeen(seen.get());
    SystemControlFamily family;
    family.name = "call";
    family.owns = [](const ThreadMessage& m) { return m.payload_json.find("test_signal") != std::string::npos; };
    family.on_lane = [](const ThreadMessage&) { return true; };
    family.apply = [this](ThreadMessage&, const SystemControlContext&) -> Roe<void> {
      ++landed;
      return {};
    };
    controls.Bind(family);
  }

  /** Learn `peer`'s published keys (as the directory hands them out). */
  void Know(const Side& peer) {
    PeerSigningKeyRecord record;
    record.signing_public_key_b64 = peer.identity->Get()->account_signing_public_key_b64;
    record.source = "test";
    signing.Put(ContactIdKindToString(mesh::ContactIdKind::Account), peer.account, record);
    peer_kem[peer.account] = *peer.identity->GetHybridKemPublicKeyB64();
  }

  /** A lane signal to `peer`, as SendControl builds it (opening the session when there is none). */
  RelayEnvelope Signal(const Side& peer) {
    ChatTargetKey target;
    target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
    target.peer_identity_value = peer.account;
    target.channel = CryptoChannel::E2ePublic;
    auto record = psk->Load(target);
    const uint32_t epoch = record && record->has_value() && (*record)->session_epoch > 0 ? (*record)->session_epoch : 1;
    auto kem = Base64Decode(peer_kem.at(peer.account));
    auto session = EnsureE2ePublicMasterPsk(*psk, target, epoch, *kem);
    EXPECT_TRUE(session) << session.error().message;

    ThreadMessage control;
    control.id = util::GenerateUuid();
    control.content_type = ChatContentType::System;
    control.text = "signal";
    control.payload_json = R"({"control_type":"test_signal","detail":"{}"})";
    control.timestamp = util::NowUnixMs();
    ControlLaneSendParams params;
    params.sender_account_id = account;
    params.sender_relay_id = "relay:" + tag;
    params.peer_account_id = peer.account;
    params.route_party = "relay:" + peer.tag;
    params.session_epoch = epoch;
    params.master_psk = session->master_psk;
    params.key_init_b64 = session->key_init_b64;
    params.order_key = 1;
    auto envelope = BuildControlLaneEnvelope(params, control, [this](const std::vector<uint8_t>& bytes) {
      return identity->SignBytes(bytes);
    });
    EXPECT_TRUE(envelope) << envelope.error().message;
    return *envelope;
  }

  /** Receive `envelope`; true when its signal landed. */
  bool Receive(const RelayEnvelope& envelope) {
    const int before = landed;
    (void)pipeline->ProcessEnvelope(envelope, account);
    return landed > before;
  }

  std::string tag;
  uint8_t seed;
  std::filesystem::path root;
  std::unique_ptr<IdentityStore> identity;
  std::string account;
  PeerSigningKeyStore signing;
  PeerSigningKeyResolver resolver{signing};
  std::unordered_map<std::string, std::string> peer_kem;
  SystemControlRegistry controls;
  std::unique_ptr<SqliteThreadStore> store;
  std::unique_ptr<GroupRosterStore> roster;
  std::unique_ptr<SqlitePskSessionStore> psk;
  std::unique_ptr<ControlLaneSeenStore> seen;
  std::unique_ptr<RelayReceivePipeline> pipeline;
  int landed = 0;
};

class E2ePublicSessionTest : public ::testing::Test {
protected:
  void SetUp() override {
    EnsureSodiumInit();
    alice_ = std::make_unique<Side>("alice", 0x10);
    bob_ = std::make_unique<Side>("bob", 0x40);
    alice_->Know(*bob_);
    bob_->Know(*alice_);
  }

  std::unique_ptr<Side> alice_;
  std::unique_ptr<Side> bob_;
};

TEST_F(E2ePublicSessionTest, OneSideOpensAndBothDirectionsWork) {
  EXPECT_TRUE(bob_->Receive(alice_->Signal(*bob_))) << "the opening send carries its key_init";
  EXPECT_TRUE(bob_->Receive(alice_->Signal(*bob_)));
  EXPECT_TRUE(alice_->Receive(bob_->Signal(*alice_))) << "the answer rides the session Bob derived";
}

TEST_F(E2ePublicSessionTest, BothSidesOpeningAtOnceStillGetThrough) {
  // Alice and Bob call each other with no session yet: each opens one before seeing the other's.
  const RelayEnvelope from_alice = alice_->Signal(*bob_);
  const RelayEnvelope from_bob = bob_->Signal(*alice_);
  EXPECT_TRUE(bob_->Receive(from_alice));
  EXPECT_TRUE(alice_->Receive(from_bob));
  // And they keep talking afterwards.
  EXPECT_TRUE(bob_->Receive(alice_->Signal(*bob_)));
  EXPECT_TRUE(alice_->Receive(bob_->Signal(*alice_)));
}

TEST_F(E2ePublicSessionTest, LosingTheOpeningSendDoesNotLockThePairOut) {
  (void)alice_->Signal(*bob_);  // opened the session; this send never arrived
  EXPECT_TRUE(bob_->Receive(alice_->Signal(*bob_))) << "the next send must still be readable";
}

TEST_F(E2ePublicSessionTest, ASideThatStartsOverIsHeardAgain) {
  ASSERT_TRUE(bob_->Receive(alice_->Signal(*bob_)));
  alice_->OpenProfile("profile-reinstalled");  // same account, empty session store
  EXPECT_TRUE(bob_->Receive(alice_->Signal(*bob_))) << "Bob must not keep reading with the old session";
  EXPECT_TRUE(alice_->Receive(bob_->Signal(*alice_)));
}

TEST_F(E2ePublicSessionTest, SessionsWithSeveralPeersBeforeAnyThread) {
  // The lane opens sessions with no thread: each must save (a shared placeholder thread id did not).
  Side carol("carol", 0x70);
  alice_->Know(carol);
  carol.Know(*alice_);
  EXPECT_TRUE(bob_->Receive(alice_->Signal(*bob_)));
  EXPECT_TRUE(carol.Receive(alice_->Signal(carol)));
  EXPECT_TRUE(alice_->Receive(carol.Signal(*alice_)));
}

TEST_F(E2ePublicSessionTest, OnceHeardTheSenderStopsAnnouncingItsSession) {
  const RelayEnvelope opening = alice_->Signal(*bob_);
  EXPECT_TRUE(opening.body.e2e.key_init_b64.has_value());
  const RelayEnvelope still_unconfirmed = alice_->Signal(*bob_);
  EXPECT_TRUE(still_unconfirmed.body.e2e.key_init_b64.has_value()) << "Bob has not answered yet";
  ASSERT_TRUE(bob_->Receive(opening));
  ASSERT_TRUE(alice_->Receive(bob_->Signal(*alice_)));
  EXPECT_FALSE(alice_->Signal(*bob_).body.e2e.key_init_b64.has_value()) << "Bob answered under it: confirmed";
}

} // namespace
} // namespace pbr
