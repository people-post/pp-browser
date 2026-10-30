#include "lib/people/RetiredKeychainBackup.h"

// The build's version, not AppVersion.h's fallback: with the fallback this reminder would never fire.
#ifndef PP_BROWSER_VERSION
#error "retired_keychain_backup_test needs PP_BROWSER_VERSION from the build (pp_foundation_runtime_core)"
#endif
#include "foundation/runtime/AppVersion.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
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
         "RetiredKeychainBackup.*, its call in ConversationsHub), this test, the iOS Keychain store "
         "(OsSecureBlobStore, foundation/platform/os/OsSecureBlob*), and update at-rest A011.";
}

TEST(RetiredKeychainBackupTest, IsANoOpWhereTheOsHasNoStore) {
  EXPECT_TRUE(RemoveRetiredKeychainBackup());
}

} // namespace
} // namespace pbr
