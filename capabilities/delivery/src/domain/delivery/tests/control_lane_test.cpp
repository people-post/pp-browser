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
    return {};
  }
  Roe<RelayPollResult> PollInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<RelayDeleteResult> AckInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<RelayDeleteResult> ClearInbox(const std::string&, const std::string&) override { return Error("unused"); }
  Roe<ChatHistoryResponse> FetchChatHistory(const ChatHistoryRequest&) override { return Error("unused"); }
  std::vector<std::string> To() {
    std::lock_guard lock(mu_);
    return to_;
  }

private:
  std::mutex mu_;
  std::vector<std::string> to_;
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
        *identity_, *own_, *contacts_, *routes_, *keys_, []() -> IDirectMessageClient* { return nullptr; }, *outbox_);
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

TEST_F(ControlLaneTest, ASendToNobodyIsDoneAtOnce) {
  bool done = false;
  lane_->SendToMany({}, [&](std::vector<ControlSendResult> results) { done = results.empty(); });
  EXPECT_TRUE(done);
}

} // namespace
} // namespace pbr
