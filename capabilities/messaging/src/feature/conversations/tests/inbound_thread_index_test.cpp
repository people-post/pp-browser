#include "feature/conversations/InboundThreadIndex.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

TEST(InboundThreadIndexTest, FindsWhatWasRemembered) {
  InboundThreadIndex index;
  EXPECT_EQ(index.Find("m1"), std::nullopt);
  index.Remember("m1", "t1");
  EXPECT_EQ(index.Find("m1"), std::optional<std::string>("t1"));
}

TEST(InboundThreadIndexTest, EmptyIdsAreIgnored) {
  InboundThreadIndex index;
  index.Remember("", "t1");
  index.Remember("m1", "");
  EXPECT_EQ(index.Find(""), std::nullopt);
  EXPECT_EQ(index.Find("m1"), std::nullopt);
}

TEST(InboundThreadIndexTest, ARememberedIdMovesToItsLatestThread) {
  InboundThreadIndex index;
  index.Remember("m1", "t1");
  index.Remember("m1", "t2");
  EXPECT_EQ(index.Find("m1"), std::optional<std::string>("t2"));
}

TEST(InboundThreadIndexTest, TheOldestEntryGoesFirstAtCapacity) {
  InboundThreadIndex index;
  for (size_t i = 0; i < InboundThreadIndex::kCapacity; ++i) {
    index.Remember("m" + std::to_string(i), "t");
  }
  index.Remember("m0", "t-again");  // an update does not refresh its age or grow the index
  index.Remember("new", "t");
  EXPECT_EQ(index.Find("m0"), std::nullopt);
  EXPECT_EQ(index.Find("m1"), std::optional<std::string>("t"));
  EXPECT_EQ(index.Find("new"), std::optional<std::string>("t"));
}

} // namespace
} // namespace pbr
