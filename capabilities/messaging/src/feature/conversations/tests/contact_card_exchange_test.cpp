#include "lib/people/testing/InMemoryPeerCards.h"
#include "feature/conversations/ContactCardExchange.h"

#include "common/ValueJson.h"
#include "common/directory/DirectoryJson.h"
#include "lib/people/AccountCard.h"
#include "lib/messaging/tests/TestAccount.h"
#include "lib/net/SwitchableDirectoryClient.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <optional>
#include <memory>
#include <string>
#include "common/MeshAlias.h"

namespace pbr {
namespace {

constexpr int64_t kNow = 1'761'235'200'000;

class ContactCardExchangeTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_contact_cards_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    contacts_ = std::make_unique<ContactsStore>(dir_.string());
    cards_ = std::make_unique<InMemoryPeerCards>();
    keys_ = std::make_unique<AccountKeyResolver>(signing_, kem_, *cards_, directory_); // no directory
    exchange_ = std::make_unique<ContactCardExchange>(*contacts_, *keys_);
  }
  void TearDown() override {
    exchange_.reset();
    keys_.reset();
    cards_.reset();
    contacts_.reset();
    std::filesystem::remove_all(dir_);
  }

  AccountCard SignedCard(const test::TestAccount& account, std::vector<AccountCardDevice> devices) {
    AccountCard card{.keys = account.Card(), .nickname = "bob", .devices = std::move(devices), .issued_at = kNow};
    return SignAccountCard(card, [&account](const ByteVector& bytes) { return account.Sign(bytes); }).value();
  }

  ChatContactCardFields Shared(const std::string& card_json) {
    return ChatContactCardFields{.display_name = "Bob from work", .card_json = card_json};
  }

  std::filesystem::path dir_;
  PeerSigningKeyStore signing_;
  PeerKemKeyStore kem_;
  SwitchableDirectoryClient directory_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<InMemoryPeerCards> cards_;
  std::unique_ptr<AccountKeyResolver> keys_;
  std::unique_ptr<ContactCardExchange> exchange_;
  test::TestAccount bob_ = test::TestAccount::Make();
  test::TestAccount mallory_ = test::TestAccount::Make();
};

TEST_F(ContactCardExchangeTest, AddingASignedCardKeepsTheKeysAndDevices) {
  const AccountCard card = SignedCard(bob_, test::Devices({"QmBobPhone"}));
  auto contact = exchange_->Add(Shared(DumpJson(AccountCardToJson(card))), kNow);
  ASSERT_TRUE(contact) << contact.error().message;
  EXPECT_TRUE(contact->local.display_name.empty()) << "a sharer's label is never a remark";
  EXPECT_EQ(ContactEffectiveTitle(*contact), "bob");
  EXPECT_EQ(contact->remote.nickname, "bob");
  ASSERT_EQ(contact->remote.endpoints.size(), 1u);
  EXPECT_EQ(contact->remote.endpoints[0].peer_id, test::PeerIdOf("QmBobPhone"));
  const auto has = [&](mesh::ContactIdKind kind, const std::string& value) {
    for (const auto& id : contact->remote.ids) {
      if (id.kind == kind && id.value == value) {
        return true;
      }
    }
    return false;
  };
  EXPECT_TRUE(has(mesh::ContactIdKind::Account, bob_.account_id)) << "its Account ID is its relay inbox (MR001)";
  EXPECT_EQ(contact->remote.ids.size(), 2u) << "the account and its device, nothing else";
  const std::string kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  EXPECT_TRUE(kem_.Get(kind, bob_.account_id).has_value()) << "Bob can be written to with no directory";
  EXPECT_TRUE(signing_.Get(kind, bob_.account_id).has_value());
}

TEST_F(ContactCardExchangeTest, AForgedCardAddsNobody) {
  AccountCard card = SignedCard(mallory_, test::Devices({"QmMallory"}));
  card.keys.account_id = bob_.account_id; // Mallory's keys presented as Bob's
  EXPECT_FALSE(exchange_->Add(Shared(DumpJson(AccountCardToJson(card))), kNow));
  AccountCard tampered = SignedCard(bob_, test::Devices({"QmBobPhone"}));
  tampered.devices.push_back(test::NamedDevice("QmMallory").Entry());
  EXPECT_FALSE(exchange_->Add(Shared(DumpJson(AccountCardToJson(tampered))), kNow));
  EXPECT_FALSE(contacts_->FindByIdentity(bob_.account_id, mesh::ContactIdKind::Account).value().has_value());
}

TEST_F(ContactCardExchangeTest, AContactIsSharedWithItsAccountsNewestSignedCard) {
  const AccountCard card = SignedCard(bob_, test::Devices({"QmBobPhone"}));
  auto contact = exchange_->Add(Shared(DumpJson(AccountCardToJson(card))), kNow);
  ASSERT_TRUE(contact);
  auto shared = exchange_->CardFor(contact->id);
  ASSERT_TRUE(shared) << shared.error().message;
  EXPECT_EQ(shared->display_name, "bob") << "a card carries their nickname, never our remark";
  auto passed_on = AccountCardFromJson(*TryParseObject(shared->card_json));
  ASSERT_TRUE(passed_on) << "the signed card, passed on as received";
  EXPECT_TRUE(VerifyAccountCard(*passed_on, kNow));
}

TEST_F(ContactCardExchangeTest, AnInviteAddsThePersonAndFetchesTheirCardFromAListedDevice) {
  const AccountCard card = SignedCard(bob_, test::Devices({"QmBobLaptop"}));
  std::vector<std::string> asked;
  exchange_->SetCardFetcher(
      [&](const std::string& peer_id, const std::string& account_id, std::function<void(Roe<PeerCardRecord>)> done) {
        asked.push_back(peer_id);
        if (peer_id != "QmBobLaptop") {
          done(Error("not reached"));
          return;
        }
        done(keys_->AddAccountCard(card, kNow, "device"));
      });
  std::optional<Roe<ContactCardExchange::LocatorAdded>> result;
  exchange_->AddFromLocator(
      ContactLocator{.account_id = bob_.account_id, .nickname = "bob", .devices = {"QmBobPhone", "QmBobLaptop"}},
      [&](Roe<ContactCardExchange::LocatorAdded> added) { result = std::move(added); });
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(*result) << result->error().message;
  EXPECT_TRUE((*result)->keys_known);
  EXPECT_EQ(asked, (std::vector<std::string>{"QmBobPhone", "QmBobLaptop"})) << "listed devices, in turn";
  const std::string kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  EXPECT_TRUE(kem_.Get(kind, bob_.account_id).has_value());
}

TEST_F(ContactCardExchangeTest, AnInviteWithNoDeviceReachedStillAddsThePerson) {
  exchange_->SetCardFetcher([](const std::string&, const std::string&, std::function<void(Roe<PeerCardRecord>)> done) {
    done(Error("not reached"));
  });
  exchange_->SetBlockingPost([](std::function<void()> task) { task(); });
  std::optional<Roe<ContactCardExchange::LocatorAdded>> result;
  // No device reached and no directory: the answer comes back without keys.
  exchange_->AddFromLocator(ContactLocator{.account_id = bob_.account_id, .devices = {"QmBobPhone"}},
                            [&](Roe<ContactCardExchange::LocatorAdded> added) { result = std::move(added); });
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(*result) << result->error().message;
  EXPECT_FALSE((*result)->keys_known);
  ASSERT_EQ((*result)->contact.remote.endpoints.size(), 1u);
  EXPECT_EQ((*result)->contact.remote.endpoints[0].peer_id, "QmBobPhone");
}

TEST_F(ContactCardExchangeTest, ARefreshAsksTheDevicesEvenWhenACardIsKept) {
  const AccountCard card = SignedCard(bob_, test::Devices({"QmBobPhone"}));
  auto contact = exchange_->Add(Shared(DumpJson(AccountCardToJson(card))), kNow);
  ASSERT_TRUE(contact);
  std::vector<std::string> asked;
  exchange_->SetCardFetcher(
      [&](const std::string& peer_id, const std::string&, std::function<void(Roe<PeerCardRecord>)> done) {
        asked.push_back(peer_id);
        done(keys_->AddAccountCard(card, kNow, "device"));
      });
  bool refetched = true;
  exchange_->RefreshIfMissing(*contact, [&](bool kept) { refetched = kept; });
  EXPECT_FALSE(refetched);
  EXPECT_TRUE(asked.empty()) << "a kept card is not asked for again on its own";

  std::optional<ContactCardExchange::DeviceAsk> outcome;
  exchange_->AskDevices(*contact, [&](ContactCardExchange::DeviceAsk ask) { outcome = ask; });
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(*outcome, ContactCardExchange::DeviceAsk::Kept);
  EXPECT_EQ(asked, (std::vector<std::string>{test::PeerIdOf("QmBobPhone")})) << "the kept card's device";
}

TEST_F(ContactCardExchangeTest, ARefreshPressedAgainAtOnceDoesNotAskTheDevicesAgain) {
  const AccountCard card = SignedCard(bob_, test::Devices({"QmBobPhone"}));
  auto contact = exchange_->Add(Shared(DumpJson(AccountCardToJson(card))), kNow);
  ASSERT_TRUE(contact);
  int asks = 0;
  exchange_->SetCardFetcher([&](const std::string&, const std::string&, std::function<void(Roe<PeerCardRecord>)> done) {
    ++asks;
    done(Error("not reached"));
  });
  std::vector<ContactCardExchange::DeviceAsk> outcomes;
  exchange_->AskDevices(*contact, [&](ContactCardExchange::DeviceAsk ask) { outcomes.push_back(ask); });
  exchange_->AskDevices(*contact, [&](ContactCardExchange::DeviceAsk ask) { outcomes.push_back(ask); });
  EXPECT_EQ(asks, 1);
  EXPECT_EQ(outcomes, (std::vector<ContactCardExchange::DeviceAsk>{ContactCardExchange::DeviceAsk::NotReached,
                                                                   ContactCardExchange::DeviceAsk::NotReached}))
      << "the last ask's outcome stands";
}

TEST_F(ContactCardExchangeTest, ARefreshOfAContactWithNoAccountAsksNoDevice) {
  Contact contact;
  contact.remote.ids = {{mesh::ContactIdKind::PeerId, "QmSomeDevice", true}};
  std::optional<ContactCardExchange::DeviceAsk> outcome;
  exchange_->AskDevices(contact, [&](ContactCardExchange::DeviceAsk ask) { outcome = ask; });
  ASSERT_TRUE(outcome.has_value());
  EXPECT_EQ(*outcome, ContactCardExchange::DeviceAsk::NoDevice);
}

} // namespace
} // namespace pbr
