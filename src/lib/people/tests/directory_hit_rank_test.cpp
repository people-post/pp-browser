#include "lib/people/DirectoryHitRank.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

mesh::DirectoryHit Hit(const std::string& id, const std::string& nickname, const std::string& display) {
  mesh::DirectoryHit hit;
  hit.hit_id = id;
  hit.nickname = nickname;
  hit.display_name = display;
  return hit;
}

TEST(DirectoryHitRankTest, ExactThenPrefixThenServerOrder) {
  std::vector<mesh::DirectoryHit> hits = {Hit("a", "kenneth", "Kenneth Lee"), Hit("b", "ken-dev-2", ""),
                                          Hit("c", "Ken-Dev-1", "Android"), Hit("d", "", "someone"),
                                          Hit("e", "ken-dev-1b", "")};
  RankDirectoryHits(hits, "  ken-dev-1 ");
  EXPECT_EQ(hits[0].hit_id, "c") << "exact nickname, case-insensitive, trimmed";
  EXPECT_EQ(hits[1].hit_id, "e") << "prefix";
  EXPECT_EQ(hits[2].hit_id, "a") << "the rest keep the server's order";
  EXPECT_EQ(hits[3].hit_id, "b");
  EXPECT_EQ(hits[4].hit_id, "d");
}

TEST(DirectoryHitRankTest, DisplayNameCountsToo) {
  std::vector<mesh::DirectoryHit> hits = {Hit("a", "x1", "Bob"), Hit("b", "bob", "")};
  RankDirectoryHits(hits, "bob");
  EXPECT_EQ(hits[0].hit_id, "a") << "both exact: stable, server order";
  EXPECT_EQ(hits[1].hit_id, "b");
}

TEST(DirectoryHitRankTest, EmptyQueryLeavesTheOrder) {
  std::vector<mesh::DirectoryHit> hits = {Hit("b", "b", ""), Hit("a", "a", "")};
  RankDirectoryHits(hits, "");
  EXPECT_EQ(hits[0].hit_id, "b");
}

TEST(DirectoryHitRankTest, TiesBreakByAccountIdAscendingMissingLast) {
  mesh::DirectoryHit a = Hit("a", "bob", "");
  mesh::DirectoryHit b = Hit("b", "bob", "");
  mesh::DirectoryHit c = Hit("c", "bob", "");
  mesh::DirectoryHit d = Hit("d", "bob", "");
  a.account_id = "account:zzz";
  b.account_id = "account:aaa";
  d.account_id = "account:mmm";
  std::vector<mesh::DirectoryHit> hits = {a, c, b, d};
  RankDirectoryHits(hits, "bob");
  EXPECT_EQ(hits[0].hit_id, "b");
  EXPECT_EQ(hits[1].hit_id, "d");
  EXPECT_EQ(hits[2].hit_id, "a");
  EXPECT_EQ(hits[3].hit_id, "c") << "no account id sorts last";
}

TEST(DirectoryHitRankTest, MatchRankStillBeatsAccountId) {
  mesh::DirectoryHit a = Hit("a", "bobby", "");
  mesh::DirectoryHit b = Hit("b", "bob", "");
  a.account_id = "account:aaa";
  b.account_id = "account:zzz";
  std::vector<mesh::DirectoryHit> hits = {a, b};
  RankDirectoryHits(hits, "bob");
  EXPECT_EQ(hits[0].hit_id, "b");
}

TEST(NormalizeExactPeopleIdTest, PrefixedIdsAreKeptAndTrimmed) {
  EXPECT_EQ(NormalizeExactPeopleId("  account:abc \n").value(), "account:abc");
  EXPECT_EQ(NormalizeExactPeopleId("relay:u1").value(), "relay:u1");
}

TEST(NormalizeExactPeopleIdTest, BareTokenGetsAccountPrefix) {
  const std::string token = "AbCdEfGhIjKlMnOpQrStUvWxYz0123456789_-abcde";
  ASSERT_EQ(token.size(), 43u);
  EXPECT_EQ(NormalizeExactPeopleId(" " + token + " ").value(), "account:" + token);
}

TEST(NormalizeExactPeopleIdTest, WrongLengthOrCharsRejected) {
  const std::string token(43, 'a');
  EXPECT_FALSE(NormalizeExactPeopleId(token.substr(1)).has_value());
  EXPECT_FALSE(NormalizeExactPeopleId(token + "a").has_value());
  EXPECT_FALSE(NormalizeExactPeopleId(std::string(42, 'a') + "+").has_value());
  EXPECT_FALSE(NormalizeExactPeopleId(std::string(42, 'a') + "=").has_value());
  EXPECT_FALSE(NormalizeExactPeopleId(std::string(21, 'a') + " " + std::string(21, 'a')).has_value());
  EXPECT_FALSE(NormalizeExactPeopleId("ken-dev-1").has_value());
  EXPECT_FALSE(NormalizeExactPeopleId("").has_value());
}

} // namespace
} // namespace pbr
