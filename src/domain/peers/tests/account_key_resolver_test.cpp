#include "domain/peers/AccountKeyResolver.h"

#include "common/directory/DirectoryJson.h"
#include "lib/messaging/tests/TestAccount.h"
#include "lib/net/SwitchableDirectoryClient.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "common/MeshAlias.h"

namespace pbr {
namespace {

/** A directory that answers every account lookup with `hit`, counting the lookups. */
class OneEntryDirectory : public mesh::IDirectoryClient {
public:
  mesh::DirectoryHit hit;
  int lookups = 0;

  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string& /*query*/) override {
    return std::vector<mesh::DirectoryHit>{hit};
  }
  Roe<mesh::DirectoryHit> LookupRelayUser(const std::string& /*relay_user_id*/) override { return hit; }
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string& /*account_id*/) override {
    ++lookups;
    return hit;
  }
};

mesh::DirectoryHit EntryFor(const test::TestAccount& account) {
  mesh::DirectoryHit hit;
  hit.account_id = account.account_id;
  hit.signing_public_key_b64 = account.SigningPublicB64();
  hit.kem_public_key_b64 = account.KemPublicB64();
  hit.kem_binding_signature_b64 = account.KemBindingB64();
  return hit;
}

class AccountKeyResolverTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_account_keys_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    cards_ = std::make_unique<PeerCardStore>((dir_ / "profile.db").string());
    directory_backend_ = std::make_shared<OneEntryDirectory>();
    directory_.SetBackend(directory_backend_);
    resolver_ = std::make_unique<AccountKeyResolver>(signing_, kem_, *cards_, directory_);
  }
  void TearDown() override {
    resolver_.reset();
    cards_.reset();
    std::filesystem::remove_all(dir_);
  }

  const std::string kind_ = ContactIdKindToString(mesh::ContactIdKind::Account);
  test::TestAccount alice_ = test::TestAccount::Make();
  test::TestAccount mallory_ = test::TestAccount::Make();
  std::filesystem::path dir_;
  PeerSigningKeyStore signing_;
  PeerKemKeyStore kem_;
  std::unique_ptr<PeerCardStore> cards_;
  std::shared_ptr<OneEntryDirectory> directory_backend_;
  SwitchableDirectoryClient directory_;
  std::unique_ptr<AccountKeyResolver> resolver_;
};

TEST_F(AccountKeyResolverTest, TakesTheDirectorysKeysOnceAndKeepsThemAsACard) {
  directory_backend_->hit = EntryFor(alice_);
  auto kem = resolver_->Kem().Resolve(kind_, alice_.account_id);
  ASSERT_TRUE(kem) << kem.error().message;
  EXPECT_EQ(kem->kem_public_key_b64, alice_.KemPublicB64());
  auto signing = resolver_->Signing().Resolve(kind_, alice_.account_id);
  ASSERT_TRUE(signing) << signing.error().message;
  EXPECT_EQ(signing->signing_public_key_b64, alice_.SigningPublicB64());
  EXPECT_EQ(directory_backend_->lookups, 1) << "one lookup brings both keys";

  // A later run (empty key stores) finds the kept card without the directory.
  directory_.SetBackend(nullptr);
  PeerSigningKeyStore signing_after;
  PeerKemKeyStore kem_after;
  AccountKeyResolver after(signing_after, kem_after, *cards_, directory_);
  after.LoadKeptCards();
  EXPECT_TRUE(signing_after.Get(kind_, alice_.account_id).has_value());
  EXPECT_TRUE(after.Kem().Resolve(kind_, alice_.account_id));
}

TEST_F(AccountKeyResolverTest, RefusesAnEntryWhoseKeysAreNotTheAccounts) {
  directory_backend_->hit = EntryFor(alice_);
  directory_backend_->hit.kem_public_key_b64 = mallory_.KemPublicB64();  // a substituted KEM key
  EXPECT_FALSE(resolver_->Kem().Resolve(kind_, alice_.account_id));

  directory_backend_->hit = EntryFor(alice_);
  directory_backend_->hit.kem_binding_signature_b64.reset();  // no binding at all
  EXPECT_FALSE(resolver_->Kem().Resolve(kind_, alice_.account_id));

  directory_backend_->hit = EntryFor(mallory_);  // someone else's entry under Alice's id
  directory_backend_->hit.account_id = alice_.account_id;
  EXPECT_FALSE(resolver_->Signing().Resolve(kind_, alice_.account_id));
  EXPECT_FALSE(kem_.Get(kind_, alice_.account_id).has_value());
  EXPECT_FALSE(cards_->Get(alice_.account_id).value().has_value());
}

TEST_F(AccountKeyResolverTest, WithNoDirectoryOnlyKeptCardsAnswer) {
  directory_.SetBackend(nullptr);
  EXPECT_FALSE(resolver_->Kem().Resolve(kind_, alice_.account_id));

  ASSERT_TRUE(cards_->AddKeyCard(KeyCard{.account_id = alice_.account_id,
                                         .signing_public_key_b64 = alice_.SigningPublicB64(),
                                         .kem_public_key_b64 = alice_.KemPublicB64(),
                                         .kem_binding_signature_b64 = alice_.KemBindingB64()},
                                 "contact_card"));
  EXPECT_TRUE(resolver_->Kem().Resolve(kind_, alice_.account_id));
}

TEST_F(AccountKeyResolverTest, KeepsACarriedCardOnlyForTheAccountItWasCarriedFor) {
  directory_.SetBackend(nullptr);
  const Object card = KeyCardToJson(alice_.Card());

  EXPECT_FALSE(resolver_->AddCarriedCard(card, 0, "group_roster", mallory_.account_id))
      << "Alice's card under Mallory's roster row";
  EXPECT_FALSE(cards_->Get(alice_.account_id).value().has_value());

  KeyCard swapped = alice_.Card();
  swapped.kem_public_key_b64 = mallory_.KemPublicB64();
  EXPECT_FALSE(resolver_->AddCarriedCard(KeyCardToJson(swapped), 0, "group_roster", alice_.account_id))
      << "a KEM key Alice never bound";

  auto kept = resolver_->AddCarriedCard(card, 0, "group_roster", alice_.account_id);
  ASSERT_TRUE(kept) << kept.error().message;
  EXPECT_EQ(kept->source, "group_roster");
  EXPECT_TRUE(resolver_->Kem().Resolve(kind_, alice_.account_id));
}

} // namespace
} // namespace pbr
