#include "domain/delivery/ControlLane.h"

#include "domain/delivery/DeliveryOutbox.h"
#include "domain/delivery/E2ePublicSessionKeys.h"
#include "domain/delivery/PeerRouteBook.h"
#include "domain/peers/AccountKeyResolver.h"
#include "domain/peers/OwnCards.h"
#include "lib/delivery/RelayClients.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"
#include "lib/people/testing/InMemoryPeerCards.h"
#include "lib/people/tests/TestAccount.h"
#include "common/runtime/AppRuntime.h"
#include "common/Utilities.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

ByteVector TestDek() {
  EnsureSodiumInit();
  return ByteVector(kDataEncryptionKeySize, 7);
}

class NoDirectory : public mesh::IDirectoryClient {
public:
  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string&) override { return None(); }
  Roe<mesh::DirectoryHit> LookupRelayUser(const std::string&) override { return None(); }
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string&) override { return None(); }

private:
  static Error None() { return Error("Directory not configured"); }
};

/** Every peer has a KEM key: the pair's session opens. */
class AnyPeerKem final : public IPeerKemKeyResolver {
public:
  AnyPeerKem() : public_key_(HybridKem::GenerateKeyPair().value().public_key) {}
  Roe<PeerKemKeyRecord> Resolve(const std::string&, const std::string&) override {
    PeerKemKeyRecord record;
    record.kem_public_key_b64 = Base64Encode(public_key_);
    return record;
  }

private:
  ByteVector public_key_;
};

class CountingRelay final : public IRelayClient {
public:
  Roe<void> Send(const RelayEnvelope& envelope) override {
    std::lock_guard lock(mu_);
    to_.push_back(envelope.recipient_contact_id.value_or(""));
    hints_.push_back(envelope.relay_wake_hint);
    sent_.push_back(envelope);
    return {};
  }
  std::vector<RelayEnvelope> Sent() {
    std::lock_guard lock(mu_);
    return sent_;
  }
  Roe<RelayPollResult> PollInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<RelayDeleteResult> AckInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<RelayDeleteResult> ClearInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest&) override { return Error("unused"); }
  std::vector<std::string> To() {
    std::lock_guard lock(mu_);
    return to_;
  }
  std::vector<std::string> Hints() {
    std::lock_guard lock(mu_);
    return hints_;
  }

private:
  std::mutex mu_;
  std::vector<std::string> to_;
  std::vector<std::string> hints_;
  std::vector<RelayEnvelope> sent_;
};

class ControlLaneTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize(AppRuntimeConfig{});
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_control_lane_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    identity_ = std::make_unique<IdentityStore>(dir_.string(), "me");
    ASSERT_TRUE(identity_->SetDek(TestDek()));
    ASSERT_TRUE(identity_->LoadOrCreate());
    cards_ = std::make_unique<InMemoryPeerCards>();
    account_keys_ = std::make_unique<AccountKeyResolver>(signing_, kem_store_, *cards_, directory_);
    own_ = std::make_unique<OwnCards>(*identity_, *account_keys_);
    contacts_ = std::make_unique<ContactsStore>(dir_.string());
    routes_ = std::make_unique<PeerRouteBook>(*contacts_);
    store_ = std::make_unique<InMemoryDeliveryStore>();
    psk_ = std::make_unique<InMemoryPeerSessions>(*store_);
    ASSERT_TRUE(static_cast<bool>(store_->SetDek(TestDek())));
    ASSERT_TRUE(static_cast<bool>(psk_->SetDek(TestDek())));
    keys_ = std::make_unique<E2ePublicSessionKeys>(*psk_, peer_kem_);
    outbox_ = std::make_unique<DeliveryOutbox>([]() -> IDirectMessageClient* { return nullptr; });
    outbox_->SetRelayClient(&relay_);
    lane_ = std::make_unique<ControlLane>(
        *identity_, *own_, *account_keys_, *contacts_, *routes_, *keys_,
        []() -> IDirectMessageClient* { return nullptr; }, *outbox_);
  }

  void TearDown() override {
    ASSERT_TRUE(AppRuntime::DrainWorkersThenUI());
    lane_.reset();
    outbox_.reset();
    AppRuntime::Shutdown();
    keys_.reset();
    psk_.reset();
    store_.reset();
    routes_.reset();
    contacts_.reset();
    own_.reset();
    identity_.reset();
    std::filesystem::remove_all(dir_);
  }

  /** Keep `card` as the newest one of its account (signed by `owner`). */
  void KeepCard(const test::TestAccount& owner, std::vector<AccountCardDevice> devices, const int64_t issued_at) {
    AccountCard card{.keys = owner.Card(), .nickname = "peer", .devices = std::move(devices), .issued_at = issued_at};
    card = SignAccountCard(card, [&owner](const ByteVector& bytes) { return owner.Sign(bytes); }).value();
    ASSERT_TRUE(account_keys_->AddAccountCard(card, util::NowUnixMs(), "test"));
  }

  /** The peer was heard under every session we hold with it: no opener rides, so none forces the card. */
  void HeardUnderEverySession(const std::string& account) {
    auto records = psk_->List();
    ASSERT_TRUE(records);
    for (PeerSessionRecord record : *records) {
      if (record.key.peer_identity_value == account && record.auto_key_init_pending) {
        record.auto_key_init_pending.reset();
        ASSERT_TRUE(psk_->Save(record));
      }
    }
  }

  /** A quiet lane message to `account` (no opener of the caller's, not urgent, no card asked for), as sent. */
  RelayEnvelope SendQuiet(const std::string& account) {
    const size_t before = relay_.Sent().size();
    ControlSend send = To(account);
    EXPECT_TRUE(lane_->Send(send.peer_identity, send.control));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (relay_.Sent().size() == before && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const std::vector<RelayEnvelope> sent = relay_.Sent();
    EXPECT_EQ(sent.size(), before + 1);
    return sent.empty() ? RelayEnvelope{} : sent.back();
  }

  static ControlSend To(const std::string& peer) {
    ThreadMessage control;
    control.id = util::GenerateUuid();
    control.text = "signal";
    control.content_type = ChatContentType::System;
    return ControlSend{.peer_identity = peer, .control = control};
  }

  std::filesystem::path dir_;
  std::unique_ptr<IdentityStore> identity_;
  PeerSigningKeyStore signing_;
  PeerKemKeyStore kem_store_;
  NoDirectory directory_;
  std::unique_ptr<InMemoryPeerCards> cards_;
  std::unique_ptr<AccountKeyResolver> account_keys_;
  std::unique_ptr<OwnCards> own_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<PeerRouteBook> routes_;
  AnyPeerKem peer_kem_;
  std::unique_ptr<InMemoryDeliveryStore> store_;
  std::unique_ptr<InMemoryPeerSessions> psk_;
  std::unique_ptr<E2ePublicSessionKeys> keys_;
  CountingRelay relay_;
  std::unique_ptr<DeliveryOutbox> outbox_;
  std::unique_ptr<ControlLane> lane_;
};

// V058 2: a send to many seals each copy for its pair; one peer that cannot be reached does not hold up the
// others, and the caller hears once, one result per peer in its order.
TEST_F(ControlLaneTest, ASendToManyReportsEachPeerOnceInOrder) {
  routes_->NoteAccountRoute("account:bob", "relay:bob");
  routes_->NoteAccountRoute("account:dave", "relay:dave");
  std::mutex mu;
  int calls = 0;
  std::vector<ControlSendResult> got;
  lane_->SendToMany({To("account:bob"), To("account:carol"), To("account:dave")},
                    [&](std::vector<ControlSendResult> results) {
                      std::lock_guard lock(mu);
                      ++calls;
                      got = std::move(results);
                    });
  // On the results and the relay's sends, not a drain marker: the critical lane has more than one thread, so the
  // marker can run while a send still does.
  const auto until = [](const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return done();
  };
  ASSERT_TRUE(until([&] {
    std::lock_guard lock(mu);
    return calls == 1;
  }));
  ASSERT_TRUE(until([&] { return relay_.To().size() == 2u; }));
  std::lock_guard lock(mu);
  EXPECT_EQ(calls, 1);
  ASSERT_EQ(got.size(), 3u);
  EXPECT_EQ(got[0].peer_identity, "account:bob");
  EXPECT_TRUE(got[0].queued) << (got[0].queued ? "" : got[0].queued.error().message);
  EXPECT_EQ(got[1].peer_identity, "account:carol");
  EXPECT_FALSE(got[1].queued) << "no route to carol";
  EXPECT_EQ(got[2].peer_identity, "account:dave");
  EXPECT_TRUE(got[2].queued);
  auto to = relay_.To();
  std::sort(to.begin(), to.end());
  EXPECT_EQ(to, (std::vector<std::string>{"relay:bob", "relay:dave"}));
}

// An urgent invite carries its call_id as the push's wake hint; a quiet signal carries none.
TEST_F(ControlLaneTest, AnUrgentInviteCarriesItsCallIdAsTheWakeHint) {
  routes_->NoteAccountRoute("account:bob", "relay:bob");
  ControlSend invite = To("account:bob");
  invite.control.payload_json = R"({"control_type":"call_invite","detail":"{\"call_id\":\"call-123-abc\"}"})";
  invite.urgent = true;
  ControlSend quiet = invite;
  quiet.urgent = false;
  quiet.control.id = util::GenerateUuid();
  lane_->SendToMany({invite, quiet}, nullptr);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (relay_.To().size() < 2u && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  auto hints = relay_.Hints();
  std::sort(hints.begin(), hints.end());
  EXPECT_EQ(hints, (std::vector<std::string>{"", "call-123-abc"}));
}

// M024 (#555 B186): a peer whose card we hold gets one record with a copy per device on it, each under that device
// pair's session — never the account copy whichever device opened that, and never the account pair's opener.
TEST_F(ControlLaneTest, APeerWithACardGetsACopyPerDevice) {
  const test::TestAccount bob = test::TestAccount::Make();
  const test::TestDevice tablet = test::TestDevice::Make();
  AccountCard card{
      .keys = bob.Card(), .nickname = "bob", .devices = {bob.device.Entry(), tablet.Entry()}, .issued_at = 1};
  card = SignAccountCard(card, [&bob](const ByteVector& bytes) { return bob.Sign(bytes); }).value();
  ASSERT_TRUE(account_keys_->AddAccountCard(card, util::NowUnixMs(), "test"));
  routes_->NoteAccountRoute(bob.account_id, "relay:bob");

  auto account_pair = keys_->Resolve(bob.account_id); // a call's old-form media-key wrap opened it (M024 bridge)
  ASSERT_TRUE(account_pair && account_pair->key_init_b64) << (account_pair ? "" : account_pair.error().message);
  const std::string account_opener = *account_pair->key_init_b64;
  ControlSend invite = To(bob.account_id);
  ASSERT_TRUE(lane_->Send(invite.peer_identity, invite.control, true));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (relay_.Sent().empty() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const std::vector<RelayEnvelope> sent = relay_.Sent();
  ASSERT_EQ(sent.size(), 1u) << "one record for the account";
  const std::vector<RelayCopy> copies = RelayCopiesOf(sent.front());
  ASSERT_EQ(copies.size(), 2u);
  std::vector<std::string> devices;
  for (const RelayCopy& copy : copies) {
    devices.push_back(copy.recipient_device);
    EXPECT_EQ(copy.sender_seq, 0u) << "the lane is unsequenced";
    ASSERT_TRUE(copy.key_init_b64) << "each device pair's own opener, until heard";
    EXPECT_NE(*copy.key_init_b64, account_opener) << "the account pair's opener is no device's";
  }
  std::sort(devices.begin(), devices.end());
  std::vector<std::string> expected{bob.device.peer_id, tablet.peer_id};
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(devices, expected);
  EXPECT_NE(copies[0].key_init_b64, copies[1].key_init_b64) << "a session per device pair";
  EXPECT_TRUE(sent.front().body.e2e.sender_card) << "a ringing invite carries our card";
  for (const AccountCardDevice& device : card.devices) {
    PeerSessionKey pair{.peer_identity_kind = "account",
                        .peer_identity_value = bob.account_id,
                        .channel = CryptoChannel::E2ePublic,
                        .peer_device = device.peer_id};
    auto stored = psk_->Load(pair);
    ASSERT_TRUE(stored && stored->has_value()) << "the chat stream's device pair session, opened";
  }
}

// No card for the peer (a first contact): the account copy, carrying the account pair's opener while unconfirmed —
// one a call's old-form media-key wrap made (E2ePublicSessionKeys::Ensure) too, so no caller has to carry it.
TEST_F(ControlLaneTest, APeerWithNoCardGetsTheAccountCopy) {
  routes_->NoteAccountRoute("account:stranger", "relay:stranger");
  ASSERT_TRUE(keys_->Ensure("account:stranger"));
  auto opened = keys_->Resolve("account:stranger");
  ASSERT_TRUE(opened && opened->key_init_b64);
  ControlSend first = To("account:stranger");
  ASSERT_TRUE(lane_->Send(first.peer_identity, first.control));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (relay_.Sent().empty() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  const std::vector<RelayEnvelope> sent = relay_.Sent();
  ASSERT_EQ(sent.size(), 1u);
  const std::vector<RelayCopy> copies = RelayCopiesOf(sent.front());
  ASSERT_EQ(copies.size(), 1u);
  EXPECT_TRUE(copies.front().recipient_device.empty());
  ASSERT_TRUE(copies.front().key_init_b64) << "opened to the account KEM";
  EXPECT_EQ(*copies.front().key_init_b64, *opened->key_init_b64) << "the wrap's opener, not a second session";
}

// #555: our card went to Bob's account once — then Bob linked a tablet. Its first record from us carries the card
// (the tablet holds no card of ours), though Bob's phone has it and the tablet's session needs no opener.
TEST_F(ControlLaneTest, ADeviceLinkedAfterWeToldTheAccountGetsOurCard) {
  const test::TestAccount bob = test::TestAccount::Make();
  const test::TestDevice laptop = test::TestDevice::Make();
  KeepCard(bob, {bob.device.Entry(), laptop.Entry()}, 1);
  routes_->NoteAccountRoute(bob.account_id, "relay:bob");
  EXPECT_TRUE(SendQuiet(bob.account_id).body.e2e.sender_card) << "opens both device sessions";
  HeardUnderEverySession(bob.account_id);
  EXPECT_FALSE(SendQuiet(bob.account_id).body.e2e.sender_card) << "every device addressed was told: no card";

  const test::TestDevice tablet = test::TestDevice::Make();
  KeepCard(bob, {bob.device.Entry(), laptop.Entry(), tablet.Entry()}, 2);
  // The tablet wrote first (its session is the one it opened, adopted here): no opener of ours rides to it.
  ASSERT_TRUE(keys_->ResolveDevice(bob.account_id, tablet.Entry()));
  HeardUnderEverySession(bob.account_id);
  const RelayEnvelope to_tablet_too = SendQuiet(bob.account_id);
  const std::vector<RelayCopy> copies = RelayCopiesOf(to_tablet_too);
  ASSERT_EQ(copies.size(), 3u);
  EXPECT_TRUE(std::none_of(copies.begin(), copies.end(), [](const RelayCopy& c) { return c.key_init_b64; }));
  ASSERT_TRUE(to_tablet_too.body.e2e.sender_card) << "the tablet was never told our card";
  EXPECT_FALSE(SendQuiet(bob.account_id).body.e2e.sender_card) << "once: now every device was told";
}

// The single-device bridge (an account copy): a device that replaced the peer's only one was told nothing either.
TEST_F(ControlLaneTest, AnAccountCopyCarriesOurCardToADeviceNewOnThePeersCard) {
  if (!kControlLaneAccountCopyBetweenSingleDevices) {
    GTEST_SKIP() << "the M024 bridge is gone";
  }
  ASSERT_TRUE(own_->Card(util::NowUnixMs())) << "our card, listing this one device";
  const test::TestAccount bob = test::TestAccount::Make();
  KeepCard(bob, {bob.device.Entry()}, 1);
  routes_->NoteAccountRoute(bob.account_id, "relay:bob");
  const RelayEnvelope first = SendQuiet(bob.account_id);
  ASSERT_TRUE(RelayCopiesOf(first).front().recipient_device.empty()) << "two single devices: the account copy";
  HeardUnderEverySession(bob.account_id);
  EXPECT_FALSE(SendQuiet(bob.account_id).body.e2e.sender_card);

  const test::TestDevice reinstalled = test::TestDevice::Make(); // Bob's one device, linked anew
  KeepCard(bob, {reinstalled.Entry()}, 2);
  const RelayEnvelope after = SendQuiet(bob.account_id);
  ASSERT_TRUE(RelayCopiesOf(after).front().recipient_device.empty());
  EXPECT_FALSE(RelayCopiesOf(after).front().key_init_b64) << "the account pair's session goes on";
  EXPECT_TRUE(after.body.e2e.sender_card) << "the new device reads the account copy, and holds no card of ours";
}

TEST_F(ControlLaneTest, ASendToNobodyIsDoneAtOnce) {
  bool done = false;
  lane_->SendToMany({}, [&](std::vector<ControlSendResult> results) { done = results.empty(); });
  EXPECT_TRUE(done);
}

} // namespace
} // namespace pbr
