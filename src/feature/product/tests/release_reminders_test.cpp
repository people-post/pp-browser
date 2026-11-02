// What the app's release version decides: reminders that fail on purpose once a version is reached, and the
// version constants themselves (AppVersion.h, the product's).
#include "foundation/product/AppVersion.h"
#include "lib/people/RetiredKeychainBackup.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

std::array<int, 3> Numeric(const char* version) {
  std::array<int, 3> parts{0, 0, 0};
  (void)std::sscanf(version, "%d.%d.%d", &parts[0], &parts[1], &parts[2]);
  return parts;
}

// A reminder that fails on purpose (at-rest A011): the Keychain cleanup is temporary.
TEST(RetiredKeychainBackupTest, TheCleanupIsGoneByItsRemovalVersion) {
  EXPECT_LT(Numeric(AppVersionCoreString()), Numeric(kRetiredKeychainBackupRemoveAt))
      << "App version " << AppVersionCoreString() << " reached " << kRetiredKeychainBackupRemoveAt
      << ": every phone has run the cleanup by now. Remove RemoveRetiredKeychainBackup (lib/people/"
         "RetiredKeychainBackup.*, its call in Application) and this test, and update at-rest A011. "
         "Keep the iOS Keychain store (foundation/platform/os/OsSecureBlob*): multi-device M023's easy "
         "recovery tier uses the synced keychain.";
}

TEST(AppVersion, ConstantsPresent) {
  EXPECT_FALSE(std::string(AppVersionString()).empty());
  EXPECT_GE(kProtocolGen, 1);
  EXPECT_GE(kMinPeerProtocolGen, 1);
}

} // namespace
} // namespace pbr
