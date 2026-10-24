#include "feature/conversations/ContactCardExchange.h"

#include "common/ValueJson.h"
#include "common/directory/DirectoryJson.h"
#include "lib/messaging/AccountCard.h"
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
    cards_ = std::make_unique<PeerCardStore>((dir_ / "profile.db").string());
    keys_ = std::make_unique<AccountKeyResolver>(signing_, kem_, *cards_, directory_);  // no directory
    exchange_ = std::make_unique<ContactCardExchange>(*contacts_, *keys_);
  }
  void TearDown() override {
    exchange_.reset();
    keys_.reset();
    cards_.reset();
    contacts_.reset();
    std::filesystem::remove_all(dir_);
  }

  AccountCard SignedCard(const test::TestAccount& account, std::vector<std::string> devices) {
    AccountCard card{.keys = account.Card(), .nickname = "bob", .devices = std::move(devices), .issued_at = kNow};
    return SignAccountCard(card, [&account](const ByteVector& bytes) { return account.Sign(bytes); }).value();
  }

  ChatContactCardFields Shared(const std::string& card_json, const std::string& relay = "relay:bob") {
    return ChatContactCardFields{.display_name = "Bob from work", .card_json = card_json, .relay_user_id = relay};
  }

  std::filesystem::path dir_;
  PeerSigningKeyStore signing_;
  PeerKemKeyStore kem_;
  SwitchableDirectoryClient directory_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<PeerCardStore> cards_;
  std::unique_ptr<AccountKeyResolver> keys_;
  std::unique_ptr<ContactCardExchange> exchange_;
  test::TestAccount bob_ = test::TestAccount::Make();
  test::TestAccount mallory_ = test::TestAccount::Make();
};

TEST_F(ContactCardExchangeTest, AddingASignedCardKeepsTheKeysDevicesAndRoute) {
  const AccountCard card = SignedCard(bob_, {"QmBobPhone"});
  auto contact = exchange_->Add(Shared(DumpJson(AccountCardToJson(card))), kNow);
  ASSERT_TRUE(contact) << contact.error().message;
  EXPECT_EQ(contact->local.display_name, "Bob from work");
  EXPECT_EQ(contact->remote.nickname, "bob");
  ASSERT_EQ(contact->remote.endpoints.size(), 1u);
  EXPECT_EQ(contact->remote.endpoints[0].peer_id, "QmBobPhone");
  const auto has = [&](mesh::ContactIdKind kind, const std::string& value) {
    for (const auto& id : contact->remote.ids) {
      if (id.kind == kind && id.value == value) {
        return true;
      }
    }
    return false;
  };
  EXPECT_TRUE(has(mesh::ContactIdKind::Account, bob_.account_id));
  EXPECT_TRUE(has(mesh::ContactIdKind::RelayUser, "relay:bob"));
  const std::string kind = ContactIdKindToString(mesh::ContactIdKind::Account);
  EXPECT_TRUE(kem_.Get(kind, bob_.account_id).has_value()) << "Bob can be written to with no directory";
  EXPECT_TRUE(signing_.Get(kind, bob_.account_id).has_value());
}

TEST_F(ContactCardExchangeTest, AForgedCardAddsNobody) {
  AccountCard card = SignedCard(mallory_, {"QmMallory"});
  card.keys.account_id = bob_.account_id;  // Mallory's keys presented as Bob's
  EXPECT_FALSE(exchange_->Add(Shared(DumpJson(AccountCardToJson(card))), kNow));
  AccountCard tampered = SignedCard(bob_, {"QmBobPhone"});
  tampered.devices.push_back("QmMallory");
  EXPECT_FALSE(exchange_->Add(Shared(DumpJson(AccountCardToJson(tampered))), kNow));
  EXPECT_FALSE(contacts_->FindByIdentity(bob_.account_id, mesh::ContactIdKind::Account).value().has_value());
}

TEST_F(ContactCardExchangeTest, ASharersRelayHintDoesNotRerouteAKnownContact) {
  ASSERT_TRUE(exchange_->Add(Shared(DumpJson(KeyCardToJson(bob_.Card())), "relay:bob"), kNow));
  auto again = exchange_->Add(Shared(DumpJson(KeyCardToJson(bob_.Card())), "relay:mallory"), kNow);
  ASSERT_TRUE(again);
  for (const auto& id : again->remote.ids) {
    EXPECT_NE(id.value, "relay:mallory");
  }
}

TEST_F(ContactCardExchangeTest, AContactIsSharedWithItsAccountsNewestSignedCard) {
  const AccountCard card = SignedCard(bob_, {"QmBobPhone"});
  auto contact = exchange_->Add(Shared(DumpJson(AccountCardToJson(card))), kNow);
  ASSERT_TRUE(contact);
  auto shared = exchange_->CardFor(contact->id);
  ASSERT_TRUE(shared) << shared.error().message;
  EXPECT_EQ(shared->display_name, "Bob from work");
  EXPECT_EQ(shared->relay_user_id, "relay:bob");
  auto passed_on = AccountCardFromJson(*TryParseObject(shared->card_json));
  ASSERT_TRUE(passed_on) << "the signed card, passed on as received";
  EXPECT_TRUE(VerifyAccountCard(*passed_on, kNow));
}

TEST_F(ContactCardExchangeTest, AnInviteAddsThePersonAndFetchesTheirCardFromAListedDevice) {
  const AccountCard card = SignedCard(bob_, {"QmBobLaptop"});
  std::vector<std::string> asked;
  exchange_->SetCardFetcher([&](const std::string& peer_id, const std::string& account_id,
                                std::function<void(Roe<PeerCardRecord>)> done) {
    asked.push_back(peer_id);
    if (peer_id != "QmBobLaptop") {
      done(Error("not reached"));
      return;
    }
    done(keys_->AddAccountCard(card, kNow, "device"));
  });
  std::optional<Roe<ContactCardExchange::LocatorAdded>> result;
  exchange_->AddFromLocator(ContactLocator{.account_id = bob_.account_id, .nickname = "bob",
                                           .devices = {"QmBobPhone", "QmBobLaptop"}},
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

} // namespace
} // namespace pbr
