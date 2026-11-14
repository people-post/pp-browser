#include "domain/delivery/RecipientRelays.h"

#include "domain/peers/AccountKeyResolver.h"
#include "lib/people/testing/InMemoryPeerCards.h"
#include "lib/people/tests/TestAccount.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

// A recipient's relays for sending in list order (multi-relay R2): its kept card's list; and when every relay on it
// said it is not registered, its newest card — its devices first, then our relays' directories — kept only when it
// verifies, newest wins.

constexpr const char kOurs[] = "https://ours.example/api/relay";
constexpr const char kBobsOld[] = "https://old.example/api/relay";
constexpr const char kBobsNew[] = "https://new.example/api/relay";

class NoDirectory : public mesh::IDirectoryClient {
public:
  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string&) override { return Error("none"); }
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string&) override { return Error("none"); }
};

using Origins = std::vector<std::string>;

class RecipientRelaysTest : public ::testing::Test {
protected:
  void SetUp() override {
    keys_ = std::make_unique<AccountKeyResolver>(signing_, kem_, cards_, directory_);
    relays_ =
        std::make_unique<RecipientRelays>(*keys_, [this](const std::string& peer_id, const std::string& account_id,
                                                         std::function<void(Roe<PeerCardRecord>)> done) {
          asked_devices_.push_back(peer_id);
          if (!device_card_) {
            done(Error("not reached"));
            return;
          }
          done(keys_->AddAccountCard(*device_card_, 10'000, "device"));
          (void)account_id;
        });
    relays_->SetDirectories({kOurs});
    relays_->SetStoredCardFetch([this](const std::string& origin, const std::string&) -> Roe<std::optional<Object>> {
      asked_relays_.push_back(origin);
      if (!stored_card_) {
        return std::optional<Object>{};
      }
      return std::optional<Object>(AccountCardToJson(*stored_card_));
    });
  }

  AccountCard BobsCard(int64_t issued_at, Origins origins) {
    AccountCard card{.keys = bob_.Card(), .nickname = "bob", .devices = {bob_.device.Entry()}, .issued_at = issued_at};
    for (auto& origin : origins) {
      card.relays.push_back(AccountCardRelay{.origin = origin});
    }
    return SignAccountCard(card, [this](const ByteVector& bytes) { return bob_.Sign(bytes); }).value();
  }

  test::TestAccount bob_ = test::TestAccount::Make();
  PeerSigningKeyStore signing_;
  PeerKemKeyStore kem_;
  InMemoryPeerCards cards_;
  NoDirectory directory_;
  std::unique_ptr<AccountKeyResolver> keys_;
  std::unique_ptr<RecipientRelays> relays_;
  std::optional<AccountCard> device_card_;
  std::optional<AccountCard> stored_card_;
  std::vector<std::string> asked_devices_;
  std::vector<std::string> asked_relays_;
};

TEST_F(RecipientRelaysTest, AnAccountWithoutACardHasNoList) {
  EXPECT_TRUE(relays_->Of(bob_.account_id).empty());
  ASSERT_TRUE(keys_->AddKeyCard(bob_.Card(), "directory"));
  EXPECT_TRUE(relays_->Of(bob_.account_id).empty()) << "a key card lists no relays";
}

TEST_F(RecipientRelaysTest, TheKeptCardsListInItsOrder) {
  ASSERT_TRUE(keys_->AddAccountCard(BobsCard(1000, {kBobsOld, kOurs}), 10'000, "device"));
  EXPECT_EQ(relays_->Of(bob_.account_id), Origins({kBobsOld, kOurs}));
}

TEST_F(RecipientRelaysTest, ARefreshAsksTheDevicesFirst) {
  ASSERT_TRUE(keys_->AddAccountCard(BobsCard(1000, {kBobsOld}), 10'000, "device"));
  device_card_ = BobsCard(2000, {kBobsNew});
  EXPECT_EQ(relays_->Refresh(bob_.account_id), Origins({kBobsNew}));
  EXPECT_EQ(asked_devices_, Origins({bob_.device.peer_id}));
  EXPECT_TRUE(asked_relays_.empty()) << "a device's own card is the account's word";
}

TEST_F(RecipientRelaysTest, NoDeviceAnsweringAsksOurRelaysDirectories) {
  ASSERT_TRUE(keys_->AddAccountCard(BobsCard(1000, {kBobsOld}), 10'000, "device"));
  stored_card_ = BobsCard(2000, {kBobsNew});
  EXPECT_EQ(relays_->Refresh(bob_.account_id), Origins({kBobsNew}));
  EXPECT_EQ(asked_relays_, Origins({kOurs}));
}

TEST_F(RecipientRelaysTest, AnOlderStoredCardChangesNothing) {
  ASSERT_TRUE(keys_->AddAccountCard(BobsCard(2000, {kBobsOld}), 10'000, "device"));
  stored_card_ = BobsCard(1000, {kBobsNew});
  EXPECT_EQ(relays_->Refresh(bob_.account_id), Origins({kBobsOld})) << "newest wins";
}

TEST_F(RecipientRelaysTest, AStoredCardThatDoesNotVerifyIsNotKept) {
  ASSERT_TRUE(keys_->AddAccountCard(BobsCard(1000, {kBobsOld}), 10'000, "device"));
  AccountCard forged = BobsCard(2000, {kBobsNew});
  forged.relays = {AccountCardRelay{.origin = "https://evil.example/api/relay"}}; // not what Bob signed
  stored_card_ = forged;
  EXPECT_EQ(relays_->Refresh(bob_.account_id), Origins({kBobsOld}));
}

TEST_F(RecipientRelaysTest, TheRoutesAreTheseLookups) {
  ASSERT_TRUE(keys_->AddAccountCard(BobsCard(1000, {kBobsOld}), 10'000, "device"));
  stored_card_ = BobsCard(2000, {kBobsNew});
  RelayRoutes routes = relays_->Routes();
  EXPECT_EQ(routes.relays_of(bob_.account_id), Origins({kBobsOld}));
  EXPECT_EQ(routes.refresh_relays_of(bob_.account_id), Origins({kBobsNew}));
  EXPECT_EQ(routes.relays_of(bob_.account_id), Origins({kBobsNew}));
}

} // namespace
} // namespace pbr
