#include "domain/delivery/RelayGrantCoordinator.h"

#include "domain/delivery/ControlLane.h"
#include "domain/delivery/DeliveryOutbox.h"
#include "domain/delivery/E2ePublicSessionKeys.h"
#include "domain/peers/AccountKeyResolver.h"
#include "domain/peers/OwnCards.h"
#include "lib/delivery/RelayClients.h"
#include "lib/delivery/RelayGrantSigner.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/people/IdentityStore.h"
#include "lib/people/testing/InMemoryPeerCards.h"
#include "lib/people/tests/TestAccount.h"
#include "common/Utilities.h"
#include "common/runtime/AppRuntime.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/HybridKem.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace pbr {
namespace {

// Multi-relay MR005: who we grant gets a grant, renewed in time and not after a Stop; the grants given to us are kept
// only when they are the grantor's own, for us, and ride our sends to them; a refused one is asked for again.

ByteVector TestDek() {
  EnsureSodiumInit();
  return ByteVector(kDataEncryptionKeySize, 7);
}

class NoDirectory : public mesh::IDirectoryClient {
public:
  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string&) override { return Error("none"); }
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string&) override { return Error("none"); }
};

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

/** Counts the relay records per recipient (each lane notice is one). */
class CountingRelay final : public IRelayClient {
public:
  Roe<void> Send(const RelayEnvelope& envelope) override {
    std::lock_guard lock(mu_);
    ++to_[envelope.recipient_account_id.value_or("")];
    return {};
  }
  int To(const std::string& account) {
    std::lock_guard lock(mu_);
    return to_[account];
  }
  Roe<RelayPollResult> PollInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<RelayDeleteResult> AckInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<RelayDeleteResult> ClearInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest&) override { return Error("unused"); }

private:
  std::mutex mu_;
  std::map<std::string, int> to_;
};

constexpr int64_t kDayMs = 24LL * 60 * 60 * 1000;

class RelayGrantCoordinatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    AppRuntime::Initialize(AppRuntimeConfig{});
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_relay_grants_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    identity_ = std::make_unique<IdentityStore>(dir_.string(), "me");
    ASSERT_TRUE(identity_->SetDek(TestDek()));
    ASSERT_TRUE(identity_->LoadOrCreate());
    me_ = identity_->Get()->account_id;
    me_key_ = identity_->Get()->account_signing_public_key_b64;
    cards_ = std::make_unique<InMemoryPeerCards>();
    account_keys_ = std::make_unique<AccountKeyResolver>(signing_, kem_store_, *cards_, directory_);
    own_ = std::make_unique<OwnCards>(*identity_, *account_keys_);
    store_ = std::make_unique<InMemoryDeliveryStore>();
    psk_ = std::make_unique<InMemoryPeerSessions>(*store_);
    ASSERT_TRUE(static_cast<bool>(store_->SetDek(TestDek())));
    ASSERT_TRUE(static_cast<bool>(psk_->SetDek(TestDek())));
    keys_ = std::make_unique<E2ePublicSessionKeys>(*psk_, peer_kem_);
    outbox_ = std::make_unique<DeliveryOutbox>([]() -> IDirectMessageClient* { return nullptr; });
    outbox_->SetRelayClient(&relay_);
    lane_ = std::make_unique<ControlLane>(
        *identity_, *own_, *account_keys_, *keys_, []() -> IDirectMessageClient* { return nullptr; }, *outbox_);
    grants_ = std::make_unique<RelayGrantCoordinator>(*identity_, *account_keys_, *lane_);
    grants_->SetStore(&grant_store_);
    grants_->SetGrantedPeer([this](const std::string& account) {
      std::lock_guard lock(mu_);
      return granted_.contains(account);
    });
    grants_->SetClockForTest([this]() { return now_.load(); });
    ASSERT_TRUE(account_keys_->AddKeyCard(bob_.Card(), "test")) << "Bob's key, as its card brought it";
  }

  void TearDown() override {
    ASSERT_TRUE(AppRuntime::DrainWorkersThenUI());
    grants_.reset();
    lane_.reset();
    outbox_.reset();
    AppRuntime::Shutdown();
    keys_.reset();
    psk_.reset();
    store_.reset();
    own_.reset();
    identity_.reset();
    std::filesystem::remove_all(dir_);
  }

  void Grant(const std::string& account) {
    std::lock_guard lock(mu_);
    granted_.insert(account);
  }
  /** Until `account` got `count` relay records (lane notices), or false after 5 s. */
  bool SentTo(const std::string& account, const int count) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (relay_.To(account) < count && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return relay_.To(account) >= count;
  }
  /** What the relay got for `account` once the workers are done. */
  int Settled(const std::string& account) {
    EXPECT_TRUE(AppRuntime::DrainWorkersThenUI());
    return relay_.To(account);
  }
  /** Bob's grant to us, signed by Bob's account key. */
  RelayGrant BobsGrant(const int64_t issued_at) {
    return IssueRelayGrant(bob_.account_id, me_, issued_at,
                           [this](const std::vector<uint8_t>& bytes) { return bob_.Sign(bytes); })
        .value();
  }

  std::filesystem::path dir_;
  std::unique_ptr<IdentityStore> identity_;
  std::string me_;
  std::string me_key_;
  PeerSigningKeyStore signing_;
  PeerKemKeyStore kem_store_;
  NoDirectory directory_;
  std::unique_ptr<InMemoryPeerCards> cards_;
  std::unique_ptr<AccountKeyResolver> account_keys_;
  std::unique_ptr<OwnCards> own_;
  AnyPeerKem peer_kem_;
  std::unique_ptr<InMemoryDeliveryStore> store_;
  std::unique_ptr<InMemoryPeerSessions> psk_;
  std::unique_ptr<E2ePublicSessionKeys> keys_;
  CountingRelay relay_;
  std::unique_ptr<DeliveryOutbox> outbox_;
  std::unique_ptr<ControlLane> lane_;
  InMemoryRelayGrantStore grant_store_;
  std::unique_ptr<RelayGrantCoordinator> grants_;
  std::mutex mu_;
  std::set<std::string> granted_;
  std::atomic<int64_t> now_{1761235200000};
  test::TestAccount bob_ = test::TestAccount::Make();
};

TEST_F(RelayGrantCoordinatorTest, AGrantedPeerGetsAGrantOfOursOverTheLaneAndOnlyOnceUntilItIsDue) {
  Grant(bob_.account_id);
  grants_->EnsureGranted({bob_.account_id, "account:stranger", me_});
  auto issued = grant_store_.IssuedTo(bob_.account_id);
  ASSERT_TRUE(issued && *issued);
  const RelayGrant& grant = (*issued)->grant;
  EXPECT_EQ(grant.recipient_account_id, me_);
  EXPECT_EQ(grant.sender_account_id, bob_.account_id);
  EXPECT_TRUE(CheckRelayGrant(grant, me_, bob_.account_id, me_key_, now_.load())) << "signed with our account key";
  EXPECT_FALSE((*issued)->confirmed);
  EXPECT_TRUE(SentTo(bob_.account_id, 1)) << "the grant went to Bob";
  EXPECT_FALSE(grant_store_.IssuedTo("account:stranger").value()) << "nobody we do not grant";
  EXPECT_FALSE(grant_store_.IssuedTo(me_).value()) << "a self-send needs none";

  grants_->EnsureGranted({bob_.account_id});
  EXPECT_EQ(Settled(bob_.account_id), 1) << "not due: no new grant";
  EXPECT_EQ(grant_store_.IssuedTo(bob_.account_id).value()->grant, grant);

  now_ += 21 * kDayMs; // less than ten days left
  grants_->EnsureGranted({bob_.account_id});
  EXPECT_EQ(Settled(bob_.account_id), 2) << "renewed before it ran out";
  EXPECT_GT(grant_store_.IssuedTo(bob_.account_id).value()->grant.issued_at, grant.issued_at);
}

// MR005: a call we answered or are in grants its people, contacts or not (a stranger's call, a call grown past our
// contacts) — an ordinary grant, sent at once; never to a blocked account; and nobody renews it unless the peer is
// also a contact or a group member.
TEST_F(RelayGrantCoordinatorTest, ACallsPeopleAreGrantedOnceNeverIfBlockedAndNotRenewed) {
  grants_->SetBlockedPeer([](const std::string& account) { return account == "account:blocked"; });
  grants_->GrantCallPeers({bob_.account_id, "account:blocked", me_});
  auto issued = grant_store_.IssuedTo(bob_.account_id);
  ASSERT_TRUE(issued && *issued) << "Bob is no contact, but he is in our call";
  EXPECT_TRUE(CheckRelayGrant((*issued)->grant, me_, bob_.account_id, me_key_, now_.load()));
  EXPECT_EQ((*issued)->grant.expires_at - (*issued)->grant.issued_at, kRelayGrantMaxLifetimeMs);
  EXPECT_TRUE(SentTo(bob_.account_id, 1)) << "over the lane, like any grant";
  EXPECT_FALSE(grant_store_.IssuedTo("account:blocked").value()) << "a blocked account gets none";
  EXPECT_FALSE(grant_store_.IssuedTo(me_).value());

  grants_->GrantCallPeers({bob_.account_id});
  EXPECT_EQ(Settled(bob_.account_id), 1) << "the grant it holds is current: none again";

  now_ += 21 * kDayMs;
  grants_->EnsureGranted({bob_.account_id});
  EXPECT_EQ(Settled(bob_.account_id), 1) << "not a contact or a member: not renewed";
  ASSERT_TRUE(
      static_cast<bool>(grants_->OnNotice(relay_grant::Notice{.kind = relay_grant::NoticeKind::Ask}, bob_.account_id)));
  EXPECT_EQ(Settled(bob_.account_id), 2) << "asked: the same grant again while it lasts";
  EXPECT_EQ(grant_store_.IssuedTo(bob_.account_id).value()->grant, (*issued)->grant);
  now_ += 10 * kDayMs; // run out
  ASSERT_TRUE(
      static_cast<bool>(grants_->OnNotice(relay_grant::Notice{.kind = relay_grant::NoticeKind::Ask}, bob_.account_id)));
  EXPECT_EQ(Settled(bob_.account_id), 2) << "run out: a call's grant is not renewed on an ask";
}

TEST_F(RelayGrantCoordinatorTest, AStoppedPeerIsNotRenewed) {
  Grant(bob_.account_id);
  grants_->EnsureGranted({bob_.account_id});
  ASSERT_TRUE(SentTo(bob_.account_id, 1));
  {
    std::lock_guard lock(mu_);
    granted_.clear(); // blocked
  }
  grants_->Stop(bob_.account_id);
  EXPECT_FALSE(grant_store_.IssuedTo(bob_.account_id).value());
  now_ += 25 * kDayMs;
  grants_->EnsureGranted({bob_.account_id});
  grants_->NoteHeardFrom(bob_.account_id);
  ASSERT_TRUE(
      static_cast<bool>(grants_->OnNotice(relay_grant::Notice{.kind = relay_grant::NoticeKind::Ask}, bob_.account_id)));
  EXPECT_EQ(Settled(bob_.account_id), 1) << "nothing more to a blocked contact";
  EXPECT_FALSE(grant_store_.IssuedTo(bob_.account_id).value());
}

TEST_F(RelayGrantCoordinatorTest, AGrantGivenUsIsKeptAckedAndAttachedToOurSendsToItsGrantor) {
  const RelayGrant grant = BobsGrant(now_.load());
  ASSERT_TRUE(static_cast<bool>(
      grants_->OnNotice(relay_grant::Notice{.kind = relay_grant::NoticeKind::Grant, .grant = grant}, bob_.account_id)));
  EXPECT_EQ(grant_store_.Received(bob_.account_id).value(), grant);
  EXPECT_TRUE(SentTo(bob_.account_id, 1)) << "the ack";
  EXPECT_EQ(grants_->SendHooks().grant_for(bob_.account_id), grant);
  EXPECT_FALSE(grants_->SendHooks().grant_for(me_)) << "none to our own mailbox";
}

TEST_F(RelayGrantCoordinatorTest, AGrantThatIsNotTheGrantorsOwnForUsIsRefused) {
  const test::TestAccount mallory = test::TestAccount::Make();
  const RelayGrant forged = IssueRelayGrant(bob_.account_id, me_, now_.load(), [&](const std::vector<uint8_t>& bytes) {
                              return mallory.Sign(bytes);
                            }).value();
  EXPECT_FALSE(
      grants_->OnNotice(relay_grant::Notice{.kind = relay_grant::NoticeKind::Grant, .grant = forged}, bob_.account_id))
      << "not Bob's signature";
  const RelayGrant to_someone_else =
      IssueRelayGrant(bob_.account_id, "account:carol", now_.load(), [this](const std::vector<uint8_t>& bytes) {
        return bob_.Sign(bytes);
      }).value();
  EXPECT_FALSE(grants_->OnNotice(relay_grant::Notice{.kind = relay_grant::NoticeKind::Grant, .grant = to_someone_else},
                                 bob_.account_id))
      << "not for us";
  EXPECT_FALSE(grants_->OnNotice(
      relay_grant::Notice{.kind = relay_grant::NoticeKind::Grant, .grant = BobsGrant(now_.load())}, "account:carol"))
      << "only the grantor sends its grant";
  EXPECT_FALSE(grant_store_.Received(bob_.account_id).value());
}

// An ask is a stranger's send (we hold no grant), and the relay allows a stranger 3 an hour per pair: a send never
// asks, and a peer heard from is asked only once we have lacked its grant a few minutes (it sends one unasked when it
// adds us, which may be on its way), then at most every six hours.
TEST_F(RelayGrantCoordinatorTest, NoUsableGrantFromAGrantedPeerIsAskedForOnlyWhenLongLackedAndAtMostEverySixHours) {
  Grant(bob_.account_id);
  ASSERT_TRUE(grant_store_.PutIssued(
      IRelayGrantStore::Issued{.grant = RelayGrant{.recipient_account_id = me_,
                                                   .sender_account_id = bob_.account_id,
                                                   .issued_at = now_.load(),
                                                   .expires_at = now_.load() + kRelayGrantMaxLifetimeMs},
                               .confirmed = true}));
  EXPECT_FALSE(grants_->GrantFor(bob_.account_id));
  EXPECT_EQ(Settled(bob_.account_id), 0) << "a send does not ask";
  grants_->NoteHeardFrom(bob_.account_id);
  EXPECT_EQ(Settled(bob_.account_id), 0) << "just added us: its grant may be on its way";
  now_ += 11LL * 60 * 1000;
  grants_->NoteHeardFrom(bob_.account_id);
  EXPECT_TRUE(SentTo(bob_.account_id, 1)) << "lacked for minutes: asked";
  now_ += 11LL * 60 * 1000;
  grants_->NoteHeardFrom(bob_.account_id);
  EXPECT_EQ(Settled(bob_.account_id), 1) << "not again so soon";
  now_ += 7LL * 60 * 60 * 1000;
  grants_->NoteHeardFrom(bob_.account_id);
  EXPECT_EQ(Settled(bob_.account_id), 2);
  grants_->NoteHeardFrom("account:stranger");
  now_ += 11LL * 60 * 1000;
  grants_->NoteHeardFrom("account:stranger");
  EXPECT_EQ(Settled("account:stranger"), 0) << "a stranger is not asked";
}

TEST_F(RelayGrantCoordinatorTest, AGrantArrivingReleasesWhatItsGrantorsStrangerQuotaHeldBack) {
  std::vector<std::string> released;
  grants_->SetOnGrantReceived([&released](const std::string& grantor) { released.push_back(grantor); });
  ASSERT_TRUE(static_cast<bool>(grants_->OnNotice(
      relay_grant::Notice{.kind = relay_grant::NoticeKind::Grant, .grant = BobsGrant(now_.load())}, bob_.account_id)));
  EXPECT_EQ(released, std::vector<std::string>{bob_.account_id});
  EXPECT_FALSE(grants_->OnNotice(
      relay_grant::Notice{.kind = relay_grant::NoticeKind::Grant, .grant = BobsGrant(now_.load())}, "account:carol"));
  EXPECT_EQ(released.size(), 1u) << "a refused grant releases nothing";
}

TEST_F(RelayGrantCoordinatorTest, ARefusedGrantIsDroppedAndAskedForAgain) {
  Grant(bob_.account_id);
  ASSERT_TRUE(grant_store_.PutReceived(BobsGrant(now_.load())));
  grants_->GrantRefused(bob_.account_id);
  EXPECT_FALSE(grant_store_.Received(bob_.account_id).value());
  EXPECT_TRUE(SentTo(bob_.account_id, 1)) << "asked for a new one";
}

TEST_F(RelayGrantCoordinatorTest, AnUnconfirmedGrantGoesAgainWhenItsGranteeIsHeardFromAndAnAckSettlesIt) {
  Grant(bob_.account_id);
  ASSERT_TRUE(grant_store_.PutReceived(BobsGrant(now_.load()))); // we hold Bob's: nothing to ask for
  grants_->EnsureGranted({bob_.account_id});
  ASSERT_TRUE(SentTo(bob_.account_id, 1));
  now_ += 2LL * 60 * 60 * 1000;
  grants_->NoteHeardFrom(bob_.account_id);
  EXPECT_TRUE(SentTo(bob_.account_id, 2)) << "Bob is online: ours again, unconfirmed";

  const int64_t issued_at = grant_store_.IssuedTo(bob_.account_id).value()->grant.issued_at;
  ASSERT_TRUE(static_cast<bool>(grants_->OnNotice(
      relay_grant::Notice{.kind = relay_grant::NoticeKind::Ack, .issued_at = issued_at}, bob_.account_id)));
  EXPECT_TRUE(grant_store_.IssuedTo(bob_.account_id).value()->confirmed);
  now_ += 2LL * 60 * 60 * 1000;
  grants_->NoteHeardFrom(bob_.account_id);
  EXPECT_EQ(Settled(bob_.account_id), 2) << "confirmed: not sent again";
}

TEST_F(RelayGrantCoordinatorTest, AnAskFromAGrantedPeerIsAnsweredWithAGrant) {
  Grant(bob_.account_id);
  ASSERT_TRUE(
      static_cast<bool>(grants_->OnNotice(relay_grant::Notice{.kind = relay_grant::NoticeKind::Ask}, bob_.account_id)));
  EXPECT_TRUE(SentTo(bob_.account_id, 1));
  EXPECT_TRUE(grant_store_.IssuedTo(bob_.account_id).value());
}

} // namespace
} // namespace pbr
