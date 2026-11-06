#include "foundation/platform/os/KeychainStatus.h"

#include <gtest/gtest.h>

namespace pbr::os {
namespace {

// The grace window deletes on Missing only; every status it does not know must keep the item (Unavailable).
TEST(KeychainStatusTest, SuccessIsFound) {
  EXPECT_EQ(MapKeychainStatus(0), KeychainLookup::Found);
}

TEST(KeychainStatusTest, ItemNotFoundIsMissing) {
  EXPECT_EQ(MapKeychainStatus(-25300), KeychainLookup::Missing); // errSecItemNotFound
}

TEST(KeychainStatusTest, InteractionNotAllowedIsUnavailable) {
  EXPECT_EQ(MapKeychainStatus(-25308), KeychainLookup::Unavailable); // errSecInteractionNotAllowed (locked)
}

TEST(KeychainStatusTest, AnyOtherStatusIsUnavailable) {
  for (const int32_t status : {-34018, -25291, -25299, -50, -128, 1, 12345}) {
    EXPECT_EQ(MapKeychainStatus(status), KeychainLookup::Unavailable) << status;
  }
}

} // namespace
} // namespace pbr::os
