#include "domain/people/DirectoryHitRank.h"

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

TEST(DirectoryHitRankTest, ExactIdsAreRecognised) {
  EXPECT_TRUE(LooksLikeExactPeopleId("account:UCCslXOiOn3yjtg21DqDyUM483pcePONquFsSUk_LKU"));
  EXPECT_TRUE(LooksLikeExactPeopleId(" relay:abc "));
  EXPECT_FALSE(LooksLikeExactPeopleId("ken"));
  EXPECT_FALSE(LooksLikeExactPeopleId("my account: ken"));
}

} // namespace
} // namespace pbr
