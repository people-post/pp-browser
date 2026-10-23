#include "domain/peers/RelayDirectoryKemKeyResolver.h"

#include "common/directory/DirectoryJson.h"
#include "lib/messaging/tests/TestAccount.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>
#include "common/MeshAlias.h"

namespace pbr {
namespace {

/** A directory that answers every account lookup with `hit`. */
class OneEntryDirectory : public mesh::IDirectoryClient {
public:
  mesh::DirectoryHit hit;

  Roe<std::vector<mesh::DirectoryHit>> SearchPeople(const std::string& /*query*/) override {
    return std::vector<mesh::DirectoryHit>{hit};
  }
  Roe<mesh::DirectoryHit> LookupRelayUser(const std::string& /*relay_user_id*/) override { return hit; }
  Roe<mesh::DirectoryHit> LookupByAccount(const std::string& /*account_id*/) override { return hit; }
};

mesh::DirectoryHit EntryFor(const test::TestAccount& account) {
  mesh::DirectoryHit hit;
  hit.account_id = account.account_id;
  hit.signing_public_key_b64 = account.SigningPublicB64();
  hit.kem_public_key_b64 = account.KemPublicB64();
  hit.kem_binding_signature_b64 = account.KemBindingB64();
  return hit;
}

class RelayDirectoryKemKeyResolverTest : public ::testing::Test {
protected:
  const std::string kind_ = ContactIdKindToString(mesh::ContactIdKind::Account);
  test::TestAccount alice_ = test::TestAccount::Make();
  test::TestAccount mallory_ = test::TestAccount::Make();
  PeerKemKeyStore store_;
  OneEntryDirectory directory_;
  RelayDirectoryKemKeyResolver resolver_{store_, directory_};
};

TEST_F(RelayDirectoryKemKeyResolverTest, TakesTheKeyTheAccountBound) {
  directory_.hit = EntryFor(alice_);
  auto resolved = resolver_.Resolve(kind_, alice_.account_id);
  ASSERT_TRUE(resolved) << resolved.error().message;
  EXPECT_EQ(resolved->kem_public_key_b64, alice_.KemPublicB64());
  EXPECT_TRUE(store_.Get(kind_, alice_.account_id).has_value());
}

TEST_F(RelayDirectoryKemKeyResolverTest, RefusesAnEntryWhoseKeyTheAccountDidNotBind) {
  directory_.hit = EntryFor(alice_);
  directory_.hit.kem_public_key_b64 = mallory_.KemPublicB64();  // a substituted key
  EXPECT_FALSE(resolver_.Resolve(kind_, alice_.account_id));

  directory_.hit = EntryFor(alice_);
  directory_.hit.kem_binding_signature_b64.reset();  // no binding at all
  EXPECT_FALSE(resolver_.Resolve(kind_, alice_.account_id));

  directory_.hit = EntryFor(mallory_);  // someone else's entry under Alice's id
  directory_.hit.account_id = alice_.account_id;
  EXPECT_FALSE(resolver_.Resolve(kind_, alice_.account_id));
  EXPECT_FALSE(store_.Get(kind_, alice_.account_id).has_value());
}

} // namespace
} // namespace pbr
