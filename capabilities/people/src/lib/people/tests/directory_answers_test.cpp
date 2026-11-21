#include "lib/people/DirectoryAnswers.h"

#include "lib/people/tests/TestAccount.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace pbr {
namespace {

// A name search across our relays (multi-relay R2; DESIGN "A name search asks the relays the user has listed"):
// one list, the preferred relay's first, each account once, its entry the one that proves itself.

mesh::DirectoryHit Entry(const std::string& account_id, const std::string& nickname, int64_t updated_at = 1) {
  mesh::DirectoryHit hit;
  hit.hit_id = account_id;
  hit.account_id = account_id;
  hit.nickname = nickname;
  hit.endpoints.push_back(mesh::DirectoryEndpoint{.peer_id = "peer-" + nickname, .updated_at = updated_at});
  return hit;
}

mesh::DirectoryHit EntryFor(const test::TestAccount& account, const std::string& nickname, int64_t updated_at = 1) {
  mesh::DirectoryHit hit = Entry(account.account_id, nickname, updated_at);
  hit.signing_public_key_b64 = account.SigningPublicB64();
  hit.kem_public_key_b64 = account.KemPublicB64();
  hit.kem_binding_b64 = account.KemBindingB64();
  return hit;
}

DirectoryAnswer Answered(const std::string& origin, std::vector<mesh::DirectoryHit> hits) {
  return DirectoryAnswer{.origin = origin, .hits = std::move(hits), .error = ""};
}

DirectoryAnswer Unreached(const std::string& origin) {
  return DirectoryAnswer{.origin = origin, .hits = std::nullopt, .error = "timed out"};
}

std::vector<std::string> Nicknames(const std::vector<mesh::DirectoryHit>& hits) {
  std::vector<std::string> out;
  for (const mesh::DirectoryHit& hit : hits) {
    out.push_back(hit.nickname);
  }
  return out;
}

TEST(DirectoryAnswersTest, ThePreferredRelaysOrderComesFirstThenEachNextRelaysNewAccounts) {
  const auto merged = MergeDirectoryAnswers({
      Answered("https://a.example", {Entry("account:b", "bob"), Entry("account:a", "alice")}),
      Answered("https://b.example",
               {Entry("account:c", "carol"), Entry("account:a", "alice"), Entry("account:d", "dan")}),
  });
  EXPECT_EQ(Nicknames(merged), (std::vector<std::string>{"bob", "alice", "carol", "dan"}))
      << "an account seen on both is listed once, where the preferred relay put it";
}

TEST(DirectoryAnswersTest, ARelayThatGaveNoAnswerAddsNothingAndFailsNothing) {
  const auto merged = MergeDirectoryAnswers({
      Unreached("https://a.example"),
      Answered("https://b.example", {Entry("account:c", "carol")}),
      Unreached("https://c.example"),
  });
  EXPECT_EQ(Nicknames(merged), (std::vector<std::string>{"carol"})) << "the preferred relay down: the others' hits";
  EXPECT_TRUE(MergeDirectoryAnswers({Unreached("https://a.example")}).empty());
}

TEST(DirectoryAnswersTest, AnEntryWhoseKeysNeedANewerAppDoesNotVerify) {
  const test::TestAccount alice = test::TestAccount::Make();
  mesh::DirectoryHit hit = EntryFor(alice, "alice");
  ASSERT_TRUE(DirectoryHitKeysVerify(hit));
  hit.keys_update_needed = true;
  EXPECT_FALSE(DirectoryHitKeysVerify(hit));
}

TEST(DirectoryAnswersTest, AnEntryWhoseKeysVerifyWinsOverOneWhoseDoNot) {
  const test::TestAccount alice = test::TestAccount::Make();
  const test::TestAccount mallory = test::TestAccount::Make();
  mesh::DirectoryHit forged = EntryFor(alice, "alice-forged", 9);
  forged.signing_public_key_b64 = mallory.SigningPublicB64(); // does not hash to alice's Account ID
  forged.kem_binding_b64 = mallory.KemBindingB64();
  const mesh::DirectoryHit honest = EntryFor(alice, "alice", 1);
  ASSERT_FALSE(DirectoryHitKeysVerify(forged));
  ASSERT_TRUE(DirectoryHitKeysVerify(honest));

  auto merged =
      MergeDirectoryAnswers({Answered("https://a.example", {forged}), Answered("https://b.example", {honest})});
  ASSERT_EQ(merged.size(), 1u);
  EXPECT_EQ(merged[0].nickname, "alice") << "the preferred relay's entry does not prove itself; the other's does";

  merged = MergeDirectoryAnswers({Answered("https://a.example", {honest}), Answered("https://b.example", {forged})});
  ASSERT_EQ(merged.size(), 1u);
  EXPECT_EQ(merged[0].nickname, "alice") << "a newer entry with keys that do not verify replaces nothing";
}

TEST(DirectoryAnswersTest, BothVerifyingTheEarlierRelaysEntryStaysWhateverItsEndpointsSay) {
  // An endpoint's updated_at is the relay's word, not signed: a later relay of ours claiming newer endpoints does not
  // replace the preferred relay's name, photo and addresses.
  const test::TestAccount alice = test::TestAccount::Make();
  auto merged = MergeDirectoryAnswers({Answered("https://a.example", {EntryFor(alice, "alice-a", 1)}),
                                       Answered("https://b.example", {EntryFor(alice, "alice-b", 5)})});
  ASSERT_EQ(merged.size(), 1u);
  EXPECT_EQ(merged[0].nickname, "alice-a") << "the earlier (preferred-first) directory's";

  merged = MergeDirectoryAnswers({Answered("https://a.example", {EntryFor(alice, "alice-a", 3)}),
                                  Answered("https://b.example", {EntryFor(alice, "alice-b", 3)})});
  ASSERT_EQ(merged.size(), 1u);
  EXPECT_EQ(merged[0].nickname, "alice-a");
}

TEST(DirectoryAnswersTest, AHitWithNoAccountIsKeptOncePerHitId) {
  mesh::DirectoryHit anonymous;
  anonymous.hit_id = "h1";
  anonymous.nickname = "someone";
  const auto merged = MergeDirectoryAnswers({Answered("https://a.example", {anonymous}),
                                             Answered("https://b.example", {anonymous, Entry("account:a", "a")})});
  EXPECT_EQ(Nicknames(merged), (std::vector<std::string>{"someone", "a"}));
  EXPECT_FALSE(DirectoryHitKeysVerify(anonymous));
}

} // namespace
} // namespace pbr
