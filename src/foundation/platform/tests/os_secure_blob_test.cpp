#include "foundation/platform/os/OsSecureBlob.h"

#include <gtest/gtest.h>

// Desktop and test hosts have no uninstall-surviving store: the stub must say so, read as "nothing
// stored" and delete without error, so its callers can treat it like an empty store.
TEST(OsSecureBlobTest, UnsupportedPlatformReadsEmptyAndDeletesQuietly) {
  if (pbr::os::SecureBlobSupported()) {
    GTEST_SKIP() << "real secure store on this platform";
  }
  EXPECT_FALSE(static_cast<bool>(pbr::os::SecureBlobWrite("x", {1, 2, 3})));
  auto read = pbr::os::SecureBlobRead("x");
  ASSERT_TRUE(static_cast<bool>(read));
  EXPECT_FALSE(read->has_value());
  EXPECT_TRUE(static_cast<bool>(pbr::os::SecureBlobDelete("x")));
}
