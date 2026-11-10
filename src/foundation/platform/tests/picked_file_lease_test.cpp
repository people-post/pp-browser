// PickedFileLease: a picked or captured file is released once, whichever way the flow holding it ends.
#include "foundation/platform/PickedFileLease.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

namespace pbr {
namespace {

TEST(PickedFileLeaseTest, ReleasesOnceWhenTheLastHolderLetsGo) {
  std::vector<std::string> released;
  auto lease = std::make_shared<PickedFileLease>("/tmp/a.jpg", [&](const std::string& p) { released.push_back(p); });
  auto second = lease; // a later page can be handed the same lease
  lease.reset();
  EXPECT_TRUE(released.empty()) << "still held";
  second.reset();
  ASSERT_EQ(released.size(), 1u);
  EXPECT_EQ(released[0], "/tmp/a.jpg");
}

TEST(PickedFileLeaseTest, AnExplicitReleaseIsNotRepeatedByTheDestructor) {
  int count = 0;
  {
    PickedFileLease lease("/tmp/b.jpg", [&](const std::string&) { ++count; });
    lease.Release();
    lease.Release();
    EXPECT_EQ(count, 1);
    EXPECT_EQ(lease.path(), "/tmp/b.jpg");
  }
  EXPECT_EQ(count, 1);
}

TEST(PickedFileLeaseTest, AnEmptyPathReleasesNothing) {
  int count = 0;
  {
    PickedFileLease lease("", [&](const std::string&) { ++count; });
  }
  EXPECT_EQ(count, 0);
}

} // namespace
} // namespace pbr
