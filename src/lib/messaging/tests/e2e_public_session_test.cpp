// The pair's e2e_public session as two real sides use it: each side opens it on its first send
// (AutoKey to the peer's KEM key) and the peer derives it from that send's key_init. These are the
// ways real calls stop getting through: both sides open at once, the opening send is lost, one side
// starts over (reinstall / wiped store).

#include "common/chat/MessagingJson.h"
#include "common/chat/RelayStreamKey.h"
#include "common/chat/SystemControl.h"
#include "lib/messaging/ControlLaneEnvelope.h"
#include "lib/messaging/ControlLaneSeenStore.h"
#include "lib/messaging/E2ePublicSessionLogic.h"
#include "lib/messaging/E2eRelayPayloadCodec.h"
#include "lib/messaging/EnvelopeSigner.h"
#include "lib/messaging/GroupE2ePayloadCodec.h"
#include "lib/messaging/OutboundMessage.h"
#include "lib/messaging/GroupRosterStore.h"
#include "lib/messaging/PeerSigningKeyStore.h"
#include "lib/messaging/SqlitePskSessionStore.h"
#include "lib/messaging/SqliteThreadStore.h"
#include "lib/messaging/RelayReceivePipeline.h"
#include "lib/messaging/tests/TestAccount.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/PskSessionRestart.h"
#include "common/directory/DirectoryJson.h"
#include "common/Utilities.h"

#include <algorithm>
#include <filesystem>
#include <map>
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

/**
 * One device: its identity, stores and receive pipeline; a lane family that counts what lands. With
 * `linked_to`, a linked device of that one's account (M021): the account's keys, a device of its own.
 */
struct Side {
  explicit Side(const std::string& tag, uint8_t seed, const Side* linked_to = nullptr) : tag(tag), seed(seed) {
    root = std::filesystem::temp_directory_path() / ("pp_session_" + tag + "_" + util::GenerateUuid());
    std::filesystem::create_directories(root);
    keys = linked_to ? linked_to->keys : test::TestAccount::Make();
    if (linked_to) {
      keys.device = test::TestDevice::Make();
      keys.device_public = keys.device.signing_public;
    }
    account = keys.account_id;
    OpenProfile("profile-1");
  }

  ~Side() {
    pipeline.reset();
    seen.reset();
    psk.reset();
    roster.reset();
    store.reset();
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
    pipeline = std::make_unique<RelayReceivePipeline>(
        *store,
        [this](const std::string& account_id) -> std::optional<AccountCard> {
          auto found = cards.find(account_id);
          return found == cards.end() ? std::nullopt : std::optional<AccountCard>(found->second);
        },
        *psk, keys.Keys(), *roster);
    pipeline->SetSystemControls(&controls);
    pipeline->SetControlLaneSeen(seen.get());
    chat_seq.clear();  // a reinstall numbers its messages from 1 again
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

  /** Learn `peer`'s account card (its one device) and published keys (as the directory hands them out). */
  void Know(const Side& peer) { KnowAccount({&peer}); }

  /** Learn an account's card listing `devices` (one account's sides), as its newest card says. */
  void KnowAccount(const std::vector<const Side*>& devices, const int64_t issued_at = 1) {
    cards[devices.front()->account] = CardOf(devices, issued_at);
    peer_kem[devices.front()->account] = devices.front()->keys.KemPublicB64();
  }

  /** The account card listing `devices` (all one account's), signed by the account. */
  static AccountCard CardOf(const std::vector<const Side*>& devices, const int64_t issued_at = 1) {
    AccountCard card{.keys = devices.front()->keys.Card(), .nickname = "test", .issued_at = issued_at};
    for (const Side* device : devices) {
      card.devices.push_back(device->keys.device.Entry());
    }
    return SignAccountCard(card, [&devices](const ByteVector& bytes) { return devices.front()->keys.Sign(bytes); })
        .value();
  }

  /** Who we are on what we send. */
  EnvelopeSender Sender() const {
    return EnvelopeSender{"relay:" + tag, account, keys.device.peer_id,
                          carry_keys ? std::optional<Object>(AccountCardToJson(own_card ? *own_card : keys.DeviceCard()))
                                     : std::nullopt};
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
    params.sender_device = keys.device.peer_id;
    params.sender_card = Sender().card;
    params.order_key = 1;
    auto envelope = BuildControlLaneEnvelope(params, control, [this](const std::vector<uint8_t>& bytes) {
      return keys.device.Sign(bytes);
    });
    EXPECT_TRUE(envelope) << envelope.error().message;
    return *envelope;
  }

  /** A chat message to `peer` on the public channel, as a send builds it (opening the session when there is none). */
  RelayEnvelope Chat(const Side& peer, const std::string& text) {
    ChatTargetKey target;
    target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
    target.peer_identity_value = peer.account;
    target.channel = CryptoChannel::E2ePublic;
    auto record = psk->Load(target);
    const uint32_t epoch = record && record->has_value() ? (*record)->session_epoch : 1;
    auto kem = Base64Decode(peer_kem.at(peer.account));
    auto session = EnsureE2ePublicMasterPsk(*psk, target, epoch, *kem);
    EXPECT_TRUE(session) << session.error().message;

    RelayEnvelope envelope;
    envelope.envelope_version = kRelayEnvelopeVersion;
    envelope.message_id = util::GenerateUuid();
    envelope.sender_contact_id = account;
    envelope.sender_relay_id = "relay:" + tag;
    envelope.recipient_contact_id = "relay:" + peer.tag;
    envelope.route.kind = "direct";
    envelope.route.channel = ThreadChannel::E2ePublic;
    envelope.sender_seq = ++chat_seq[epoch];
    envelope.session_epoch = epoch;
    envelope.timestamp = util::NowUnixMs();
    E2eEncryptParams params;
    params.text = text;
    params.channel = CryptoChannel::E2ePublic;
    params.peer_contact_id = peer.account;
    params.sender_contact_id = account;
    params.message_id = envelope.message_id;
    params.sender_seq = envelope.sender_seq;
    params.session_epoch = epoch;
    params.timestamp = envelope.timestamp;
    auto payload = E2eRelayPayloadCodec::EncryptText(params, session->master_psk);
    EXPECT_TRUE(payload) << payload.error().message;
    envelope.body.e2e.payload_b64 = *payload;
    if (session->key_init_b64) {
      envelope.body.e2e.key_init_b64 = *session->key_init_b64;
    }
    keys.SignEnvelope(envelope);
    return envelope;
  }

  /**
   * A chat message to `peer`'s account as SendUserMessage builds it once the peer's card is known (M021):
   * one copy per device on the card, each numbered and sealed in its device pair. Keyed by device.
   */
  std::map<std::string, RelayEnvelope> DeviceChat(const std::string& peer_account, const std::string& text) {
    ThreadMessage message;
    message.id = util::GenerateUuid();
    message.text = text;
    message.timestamp = util::NowUnixMs();
    std::map<std::string, RelayEnvelope> out;
    for (const AccountCardDevice& device : cards.at(peer_account).devices) {
      const ChatTargetKey pair = GroupE2ePayloadCodec::PairTargetKey(peer_account, device.peer_id);
      if (auto record = psk->Load(pair); record && record->has_value() && (*record)->restart_pending) {
        const uint32_t old_epoch = (*record)->session_epoch;
        EXPECT_TRUE(RestartE2ePublicSession(*psk, pair, old_epoch, old_epoch + 1, util::NowUnixMs()));
      }
      auto copy = SealPairCopy(message, account, SendRelayOptions{}, *psk, pair,
                               Base64Decode(device.kem_public_key_b64).value());
      EXPECT_TRUE(copy) << copy.error().message;
      RelayEnvelope envelope =
          BuildDirectEnvelope(message, Sender(), ThreadChannel::E2ePublic, "relay:" + peer_account,
                              {DeviceCopy(device.peer_id, std::move(*copy))}, NextRelayOrderKey());
      keys.SignEnvelope(envelope);
      out[device.peer_id] = envelope;
    }
    return out;
  }

  /** As DeviceChat, but as the relay carries it (M022): one record with every device's copy. */
  RelayEnvelope DeviceChatRecord(const std::string& peer_account, const std::string& text) {
    std::vector<RelayCopy> copies;
    RelayEnvelope first;
    for (auto& [device, envelope] : DeviceChat(peer_account, text)) {
      copies.push_back(envelope.body.e2e.copies.front());
      first = envelope;
    }
    first.body.e2e.copies = copies;
    keys.SignEnvelope(first);
    return first;
  }

  /** This device's PeerId. */
  const std::string& Device() const { return keys.device.peer_id; }

  /** Restored from an account backup: a fresh profile with the public sessions back, marked to restart. */
  void RestoreFromBackup() {
    auto sessions = psk->List();
    ASSERT_TRUE(sessions);
    OpenProfile("profile-restored-" + util::GenerateUuid());
    for (const PskSessionRecord& record : *sessions) {
      ASSERT_TRUE(psk->Save(record));
    }
    ASSERT_TRUE(MarkPublicSessionsForRestart(*psk));
  }

  /** What a send does first in a pair marked to restart (MeshDeliveryOrchestrator::StartNextEpochIfRestored). */
  void RestartIfPending(const Side& peer) {
    ChatTargetKey target;
    target.peer_identity_kind = ContactIdKindToString(mesh::ContactIdKind::Account);
    target.peer_identity_value = peer.account;
    target.channel = CryptoChannel::E2ePublic;
    auto record = psk->Load(target);
    ASSERT_TRUE(record && record->has_value());
    if ((*record)->restart_pending) {
      const uint32_t old_epoch = (*record)->session_epoch;
      ASSERT_TRUE(RestartE2ePublicSession(*psk, target, old_epoch, old_epoch + 1, util::NowUnixMs()));
    }
  }

  /** A group this side is in: the roster (owner first) and the group's thread. */
  void JoinGroup(const std::string& group_id, const std::vector<const Side*>& members) {
    GroupMetadata metadata;
    metadata.group_id = group_id;
    metadata.owner_identity = members.front()->account;
    metadata.title = "group";
    ASSERT_TRUE(roster->UpsertMetadata(metadata));
    std::vector<std::string> ids;
    for (const Side* m : members) {
      GroupRosterMember member;
      member.member_identity = m->account;
      member.contact_id = m->account;
      member.role = m == members.front() ? MemberRole::Owner : MemberRole::Member;
      ASSERT_TRUE(roster->UpsertMember(group_id, member));
      ids.push_back(m->account);
    }
    ASSERT_TRUE(store->FindOrCreateGroupThread(group_id, "group", ids));
  }

  /** A group message to `members`, one envelope each, as SendGroupMessage builds them: each copy numbered
   * in its pair (a pair this side lost its count in restarts first). */
  std::map<std::string, RelayEnvelope> GroupChat(const std::string& group_id, const std::vector<const Side*>& members,
                                                 const std::string& text, const bool per_device = false) {
    ThreadMessage message;
    message.id = util::GenerateUuid();
    message.timestamp = util::NowUnixMs();
    std::vector<GroupMemberTarget> targets;
    for (const Side* m : members) {
      if (m == this) {
        continue;
      }
      if (m->account == account) {
        continue;  // a sibling: never a copy to our own account here (M021 d3)
      }
      if (per_device) {
        // One copy per device on the member's card, numbered in its device pair (M021).
        if (std::any_of(targets.begin(), targets.end(), [&m](const GroupMemberTarget& t) {
              return t.member_identity == m->account;
            })) {
          continue;
        }
        for (const AccountCardDevice& device : cards.at(m->account).devices) {
          GroupMemberTarget target{m->account, GroupE2ePayloadCodec::PairTargetKey(m->account, device.peer_id)};
          auto record = psk->Load(target.target_key);
          target.session_epoch = record && record->has_value() ? (*record)->session_epoch : 1;
          target.sender_seq = ++group_seq[{m->account + "/" + device.peer_id, target.session_epoch}];
          targets.push_back(target);
        }
        continue;
      }
      GroupMemberTarget target{m->account, GroupE2ePayloadCodec::PairTargetKey(m->account)};
      if (auto record = psk->Load(target.target_key); record && record->has_value() && (*record)->restart_pending) {
        RestartIfPending(*m);
      }
      target.session_epoch = PairEpoch(*m) > 0 ? PairEpoch(*m) : 1;
      target.sender_seq = ++group_seq[{m->account, target.session_epoch}];
      targets.push_back(target);
    }
    auto encrypted = GroupE2ePayloadCodec::EncryptForMembers(
        text, group_id, account, message.id, message.timestamp, targets, *psk, [this](const ChatTargetKey& key) {
          if (!key.peer_device.empty()) {
            return Base64Decode(FindCardDevice(cards.at(key.peer_identity_value), key.peer_device)->kem_public_key_b64);
          }
          return Base64Decode(peer_kem.at(key.peer_identity_value));
        });
    EXPECT_TRUE(encrypted) << encrypted.error().message;
    std::map<std::string, RelayEnvelope> out;
    for (size_t i = 0; i < targets.size(); ++i) {
      const GroupMemberTarget& target = targets[i];
      const std::string& recipient = target.member_identity;
      EXPECT_TRUE(encrypted->copies[i]);
      const RelayCopy copy{.recipient_device = target.target_key.peer_device,
                           .session_epoch = target.session_epoch,
                           .sender_seq = target.sender_seq,
                           .key_init_b64 = encrypted->copies[i]->key_init_b64,
                           .payload_b64 = encrypted->copies[i]->payload_b64};
      RelayEnvelope envelope = BuildGroupMemberEnvelope(message, Sender(), group_id, recipient, recipient, {copy},
                                                        NextRelayOrderKey());
      keys.SignEnvelope(envelope);
      out[per_device ? target.target_key.peer_device : recipient] = envelope;
    }
    return out;
  }

  /** This side's pairwise session epoch with `peer` (0: none). */
  uint32_t PairEpoch(const Side& peer) {
    auto record = psk->Load(GroupE2ePayloadCodec::PairTargetKey(peer.account));
    return record && record->has_value() ? (*record)->session_epoch : 0;
  }

  /** Receive a chat message; true when it was kept. */
  bool Keep(const RelayEnvelope& envelope) {
    return pipeline->ProcessEnvelope(envelope, account).persisted;
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
  test::TestAccount keys;
  std::string account;
  /** The account cards this device keeps (a peer's device list). */
  std::map<std::string, AccountCard> cards;
  std::unordered_map<std::string, std::string> peer_kem;
  SystemControlRegistry controls;
  std::unique_ptr<SqliteThreadStore> store;
  std::unique_ptr<GroupRosterStore> roster;
  std::unique_ptr<SqlitePskSessionStore> psk;
  std::unique_ptr<ControlLaneSeenStore> seen;
  std::unique_ptr<RelayReceivePipeline> pipeline;
  int landed = 0;
  std::map<uint32_t, uint64_t> chat_seq;  // next seq per epoch
  std::map<std::pair<std::string, uint32_t>, uint64_t> group_seq;  // our seq per (member, pair epoch)
  /** Carry our account card with a key_init (as the app does): `own_card` when set, else one listing this device. */
  bool carry_keys = false;
  std::optional<AccountCard> own_card;
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

TEST(E2ePublicSessionCardTest, AFirstMessageCarriesTheCardToAnswerIt) {
  EnsureSodiumInit();
  Side alice("alice-card", 0x10);
  Side bob("bob-card", 0x40);
  alice.Know(bob);  // Alice found Bob (directory, a card); Bob has never looked Alice up.
  alice.carry_keys = true;
  bob.carry_keys = true;
  std::vector<AccountCard> kept;
  bob.pipeline->SetOnSenderCard([&](const AccountCard& card) {
    kept.push_back(card);
    bob.cards[card.keys.account_id] = card;
    bob.peer_kem[card.keys.account_id] = card.keys.kem_public_key_b64;
  });

  EXPECT_TRUE(bob.Receive(alice.Signal(bob))) << "verified with the card it carries, no lookup";
  ASSERT_EQ(kept.size(), 1u);
  EXPECT_TRUE(FindCardDevice(kept[0], alice.Device())) << "the card lists the sending device";
  EXPECT_TRUE(alice.Receive(bob.Signal(alice))) << "Bob answers with the keys the first message brought";
}

TEST(E2ePublicSessionCardTest, ACarriedCardThatIsNotTheSendersProvesNothing) {
  EnsureSodiumInit();
  Side alice("alice-forged", 0x10);
  Side bob("bob-forged", 0x40);
  const test::TestAccount mallory = test::TestAccount::Make();
  alice.Know(bob);
  int kept = 0;
  bob.pipeline->SetOnSenderCard([&](const AccountCard&) { ++kept; });

  // Alice's own card, but the envelope signed by Mallory's device: a device her card does not list.
  alice.carry_keys = true;
  RelayEnvelope resigned = alice.Signal(bob);
  mallory.SignEnvelope(resigned);
  EXPECT_FALSE(bob.Receive(resigned));

  // Mallory's card presented as Alice's: its signing key does not hash to her Account ID.
  RelayEnvelope claimed = alice.Signal(bob);
  AccountCard as_alice = mallory.DeviceCard();
  as_alice.keys.account_id = alice.account;
  claimed.body.e2e.sender_card = AccountCardToJson(as_alice);
  mallory.SignEnvelope(claimed);
  EXPECT_FALSE(bob.Receive(claimed));
  EXPECT_EQ(kept, 0);
}

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

// #378: a device restored from the account backup has its sessions back but not its numbering. Rewinding in
// the same epoch is dropped by design (D046 first-seen); it starts the pair's next epoch instead (D014).
TEST_F(E2ePublicSessionTest, ARestoredDeviceStartsTheNextEpochAndEveryMessageArrives) {
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(bob_->Keep(alice_->Chat(*bob_, "before " + std::to_string(i))));
  }
  alice_->RestoreFromBackup();
  alice_->RestartIfPending(*bob_);
  for (int i = 0; i < 3; ++i) {
    EXPECT_TRUE(bob_->Keep(alice_->Chat(*bob_, "after " + std::to_string(i))))
        << "message " << i << " from the restored device";
  }
  auto threads = bob_->store->ListThreads();
  ASSERT_TRUE(threads);
  ASSERT_EQ(threads->size(), 1u);
  auto epoch = bob_->store->GetChatTargetSessionEpoch(threads->front().id);
  ASSERT_TRUE(epoch);
  EXPECT_EQ(*epoch, 2u) << "Bob followed Alice to the next epoch";
  EXPECT_TRUE(alice_->Keep(bob_->Chat(*alice_, "welcome back"))) << "and answers in it";
  EXPECT_TRUE(bob_->Keep(alice_->Chat(*bob_, "thanks"))) << "Alice, heard, writes on without restarting again";
}

TEST_F(E2ePublicSessionTest, RewindingInTheSameEpochIsStillDropped) {
  for (int i = 0; i < 2; ++i) {
    ASSERT_TRUE(bob_->Keep(alice_->Chat(*bob_, "before " + std::to_string(i))));
  }
  alice_->RestoreFromBackup();  // no restart: a device that rewinds where Bob holds its messages
  EXPECT_FALSE(bob_->Keep(alice_->Chat(*bob_, "rewound"))) << "first-seen keeps seq 1 (D046)";
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

namespace pbr {
namespace {

// G010/G011: a group copy is a pairwise message, numbered in the pair — the pair's session epoch and the
// sender's own seq for the group there. A member only sees its own copy, so that is all it needs; a sender
// that lost its count restarts the pair (D014), for the group as for the 1:1 chat.
TEST(GroupPairwiseKeyTest, AGroupCopyIsAPairMessageNumberedInThePair) {
  EnsureSodiumInit();
  Side alice("alice-g", 0x10);
  Side bob("bob-g", 0x40);
  Side carol("carol-g", 0x70);
  for (Side* a : {&alice, &bob, &carol}) {
    for (Side* b : {&alice, &bob, &carol}) {
      if (a != b) {
        a->Know(*b);
      }
    }
  }
  const std::string group = "group:pairwise";
  const std::vector<const Side*> everyone{&alice, &bob, &carol};
  for (Side* s : {&alice, &bob, &carol}) {
    s->JoinGroup(group, everyone);
  }

  // No pair has a session yet: the first copies open them; every copy is read, and the next ones too.
  for (int i = 0; i < 3; ++i) {
    auto sent = alice.GroupChat(group, everyone, "from alice " + std::to_string(i));
    EXPECT_TRUE(bob.Keep(sent.at(bob.account))) << "bob reads alice's message " << i;
    EXPECT_TRUE(carol.Keep(sent.at(carol.account))) << "carol reads alice's message " << i;
  }
  auto answer = bob.GroupChat(group, everyone, "from bob");
  EXPECT_FALSE(answer.at(alice.account).body.e2e.key_init_b64) << "bob kept the session alice opened";
  EXPECT_TRUE(alice.Keep(answer.at(alice.account)));
  auto from_carol = carol.GroupChat(group, everyone, "from carol");
  EXPECT_EQ(answer.at(alice.account).sender_seq, from_carol.at(alice.account).sender_seq)
      << "both start their count in their own pair";
  EXPECT_TRUE(alice.Keep(from_carol.at(alice.account))) << "one sender's seq is not another's";
  auto again = alice.GroupChat(group, everyone, "alice, heard");
  EXPECT_FALSE(again.at(bob.account).body.e2e.key_init_b64) << "heard under it: alice stops announcing it";
  EXPECT_TRUE(bob.Keep(again.at(bob.account)));

  // The pairs stay at their own epoch, and the 1:1 chat keeps reading.
  EXPECT_EQ(alice.PairEpoch(bob), 1u);
  EXPECT_EQ(bob.PairEpoch(alice), 1u);
  EXPECT_TRUE(bob.Keep(alice.Chat(bob, "a 1:1 message")));

  // Bob restores from his backup (the pair sessions back, marked to restart; his counts gone): his first
  // group copy to alice restarts their pair at epoch 2 from seq 1 — not taken for one alice already holds.
  bob.RestoreFromBackup();
  bob.JoinGroup(group, everyone);
  bob.group_seq.clear();
  auto restored = bob.GroupChat(group, everyone, "bob is back");
  EXPECT_EQ(restored.at(alice.account).session_epoch, 2u);
  EXPECT_EQ(restored.at(alice.account).sender_seq, 1u);
  EXPECT_TRUE(restored.at(alice.account).body.e2e.key_init_b64) << "the restarted pair's new session";
  EXPECT_TRUE(alice.Keep(restored.at(alice.account)));
  EXPECT_TRUE(carol.Keep(restored.at(carol.account)));
  EXPECT_EQ(alice.PairEpoch(bob), 2u) << "alice follows the pair to its next epoch";
  auto back = alice.GroupChat(group, everyone, "welcome back");
  EXPECT_EQ(back.at(bob.account).session_epoch, 2u);
  EXPECT_TRUE(bob.Keep(back.at(bob.account)));
  EXPECT_TRUE(alice.Keep(bob.Chat(alice, "and in our 1:1 chat")));
}

} // namespace
} // namespace pbr

namespace pbr {
namespace {

// M021: two devices of one account, each with its own device pairs — both may send (their numbering is
// their own pair's) and both read (each its own copy; a sibling's copy is not theirs to read).
class LinkedDevicesTest : public ::testing::Test {
protected:
  void SetUp() override {
    EnsureSodiumInit();
    phone_ = std::make_unique<Side>("alice-phone", 0x10);
    laptop_ = std::make_unique<Side>("alice-laptop", 0x20, phone_.get());
    bob_ = std::make_unique<Side>("bob", 0x40);
    phone_->Know(*bob_);
    laptop_->Know(*bob_);
    bob_->KnowAccount({phone_.get(), laptop_.get()});
  }

  /** The thread's rows on `side` (its one thread). */
  static size_t Rows(Side& side) {
    auto threads = side.store->ListThreads();
    EXPECT_TRUE(threads && threads->size() == 1);
    auto page = side.store->GetMessagesPage(threads->front().id, std::nullopt, 100);
    return page ? page->size() : 0;
  }

  std::unique_ptr<Side> phone_;
  std::unique_ptr<Side> laptop_;
  std::unique_ptr<Side> bob_;
};

TEST_F(LinkedDevicesTest, BothDevicesSendAndBothRead) {
  // Each device writes in its own pair with Bob: both start at seq 1, and Bob keeps both.
  auto from_phone = phone_->DeviceChat(bob_->account, "from the phone");
  auto from_laptop = laptop_->DeviceChat(bob_->account, "from the laptop");
  ASSERT_EQ(from_phone.size(), 1u);
  EXPECT_EQ(from_phone.begin()->second.sender_seq, from_laptop.begin()->second.sender_seq);
  EXPECT_TRUE(bob_->Keep(from_phone.at(bob_->Device())));
  EXPECT_TRUE(bob_->Keep(from_laptop.at(bob_->Device()))) << "one device's seq is not its sibling's";
  EXPECT_TRUE(bob_->Keep(phone_->DeviceChat(bob_->account, "again").at(bob_->Device())));
  EXPECT_TRUE(bob_->Keep(laptop_->DeviceChat(bob_->account, "again").at(bob_->Device())));
  EXPECT_EQ(Rows(*bob_), 4u) << "one thread with Alice, whichever device wrote";

  // Bob answers the account: a copy for each device, each read on its own device only.
  auto answer = bob_->DeviceChat(phone_->account, "hi Alice");
  ASSERT_EQ(answer.size(), 2u);
  EXPECT_TRUE(phone_->Keep(answer.at(phone_->Device())));
  EXPECT_TRUE(laptop_->Keep(answer.at(laptop_->Device())));
  const RelayReceiveOutcome sibling = phone_->pipeline->ProcessEnvelope(answer.at(laptop_->Device()), phone_->account);
  EXPECT_FALSE(sibling.persisted) << "the laptop's copy, in the shared mailbox: not the phone's to read";
  EXPECT_EQ(sibling.decision, IngestDecision::SilentDiscard);
  EXPECT_FALSE(sibling.receive_failure) << "nothing went wrong";

  // Heard under the sessions they opened: nobody keeps announcing them.
  EXPECT_FALSE(phone_->DeviceChat(bob_->account, "and on").at(bob_->Device()).body.e2e.key_init_b64);
  EXPECT_FALSE(bob_->DeviceChat(phone_->account, "and on").at(laptop_->Device()).body.e2e.key_init_b64);
}

TEST_F(LinkedDevicesTest, AccountCopiesFromBothDevicesAreBothKept) {
  // Before Alice knows Bob's devices both of hers write account copies (a first contact): each numbers its
  // own, so Bob keeps both though both are seq 1.
  const RelayEnvelope from_phone = phone_->Chat(*bob_, "hi from the phone");
  const RelayEnvelope from_laptop = laptop_->Chat(*bob_, "hi from the laptop");
  EXPECT_EQ(from_phone.sender_seq, from_laptop.sender_seq);
  EXPECT_TRUE(from_phone.recipient_device.empty()) << "an account copy";
  EXPECT_TRUE(bob_->Keep(from_phone));
  EXPECT_TRUE(bob_->Keep(from_laptop));
  EXPECT_EQ(Rows(*bob_), 2u);
}

TEST_F(LinkedDevicesTest, ACopyIsReadInItsOwnPairOnly) {
  auto from_phone = phone_->DeviceChat(bob_->account, "for Bob's device");
  // The device each copy is for is signed: re-addressing one breaks the signature.
  RelayEnvelope readdressed = from_phone.at(bob_->Device());
  readdressed.body.e2e.copies.front().recipient_device = laptop_->Device();
  EXPECT_FALSE(laptop_->Keep(readdressed));
  RelayEnvelope as_account_copy = from_phone.at(bob_->Device());
  as_account_copy.body.e2e.copies.front().recipient_device.clear();
  EXPECT_FALSE(bob_->Keep(as_account_copy)) << "not an account copy either";
  EXPECT_TRUE(bob_->Keep(from_phone.at(bob_->Device())));
}

TEST_F(LinkedDevicesTest, OneRecordCarriesEveryDevicesCopy) {
  ASSERT_TRUE(bob_->Keep(phone_->DeviceChat(bob_->account, "hi").at(bob_->Device())));
  // Bob's answer as the relay carries it: one record, a copy for each of Alice's devices.
  const RelayEnvelope record = bob_->DeviceChatRecord(phone_->account, "one record for both");
  ASSERT_EQ(record.body.e2e.copies.size(), 2u);
  const auto parsed = ParseRelayEnvelope(RelayEnvelopeToJson(record));
  ASSERT_TRUE(parsed) << parsed.error().message;
  EXPECT_TRUE(phone_->Keep(*parsed)) << "the phone reads its copy";
  EXPECT_TRUE(laptop_->Keep(*parsed)) << "and the laptop its own, from the same record";
  // A record that has no copy for a device (one the sender did not know yet) is not that device's to read.
  Side tablet("alice-tablet", 0x30, phone_.get());
  tablet.Know(*bob_);
  EXPECT_FALSE(tablet.Keep(*parsed));
}

TEST_F(LinkedDevicesTest, ANewDevicesFirstMessageBringsTheNewerDeviceList) {
  // Bob knew Alice with her phone only; the laptop is newly linked and signs itself into a newer card.
  bob_->KnowAccount({phone_.get()}, 1);
  bob_->pipeline->SetOnSenderCard([this](const AccountCard& card) { bob_->cards[card.keys.account_id] = card; });
  laptop_->carry_keys = true;
  laptop_->own_card = Side::CardOf({phone_.get(), laptop_.get()}, 2);
  EXPECT_TRUE(bob_->Keep(laptop_->DeviceChat(bob_->account, "it's me, on my laptop").at(bob_->Device())))
      << "verified with the newer card its first message carries";
  auto answer = bob_->DeviceChat(phone_->account, "hello both");
  EXPECT_EQ(answer.size(), 2u) << "Bob writes to both devices from now on";
  EXPECT_TRUE(laptop_->Keep(answer.at(laptop_->Device())));
}

TEST_F(LinkedDevicesTest, AnOlderCardDoesNotBringBackADeviceTakenOut) {
  // Bob's newest card for Alice lists the phone only (the laptop was taken out); the laptop still carries
  // the older card that listed it.
  bob_->KnowAccount({phone_.get()}, 3);
  laptop_->carry_keys = true;
  laptop_->own_card = Side::CardOf({phone_.get(), laptop_.get()}, 2);
  const RelayReceiveOutcome outcome =
      bob_->pipeline->ProcessEnvelope(laptop_->DeviceChat(bob_->account, "still here").at(bob_->Device()), bob_->account);
  EXPECT_FALSE(outcome.persisted);
  EXPECT_EQ(outcome.receive_failure, std::optional<ReceiveFailure>(ReceiveFailure::SignatureCheckFailed));
}

TEST_F(LinkedDevicesTest, GroupCopiesGoToEachMemberDevice) {
  const std::string group = "group:linked";
  const std::vector<const Side*> members{bob_.get(), phone_.get(), laptop_.get()};
  for (Side* side : {bob_.get(), phone_.get(), laptop_.get()}) {
    side->JoinGroup(group, members);
  }
  auto sent = bob_->GroupChat(group, members, "to the group", true);
  ASSERT_EQ(sent.size(), 2u) << "one copy per device of Alice's";
  EXPECT_TRUE(phone_->Keep(sent.at(phone_->Device())));
  EXPECT_TRUE(laptop_->Keep(sent.at(laptop_->Device())));
  EXPECT_FALSE(phone_->Keep(sent.at(laptop_->Device())));
  // And each of Alice's devices writes to the group in its own pairs.
  auto from_phone = phone_->GroupChat(group, members, "phone here", true);
  auto from_laptop = laptop_->GroupChat(group, members, "laptop here", true);
  EXPECT_TRUE(bob_->Keep(from_phone.at(bob_->Device())));
  EXPECT_TRUE(bob_->Keep(from_laptop.at(bob_->Device())));
}

} // namespace
} // namespace pbr

