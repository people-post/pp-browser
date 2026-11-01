#include "lib/people/RetiredKeychainBackup.h"

// The build's version, not AppVersion.h's fallback: with the fallback this reminder would never fire.
#ifndef PP_BROWSER_VERSION
#error "retired_keychain_backup_test needs PP_BROWSER_VERSION from the build (pp_foundation_runtime_core)"
#endif
#include "foundation/runtime/AppVersion.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <vector>
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

/** A supported store in memory, keeping what was deleted. */
class MemoryStore final : public ISecureBlobStore {
public:
  bool Supported() const override { return true; }
  Roe<void> Write(const std::string& name, const std::vector<uint8_t>& bytes) override {
    blobs[name] = bytes;
    return {};
  }
  Roe<std::optional<std::vector<uint8_t>>> Read(const std::string& name) override {
    auto it = blobs.find(name);
    return it == blobs.end() ? std::optional<std::vector<uint8_t>>{} : std::optional(it->second);
  }
  Roe<void> Delete(const std::string& name) override {
    blobs.erase(name);
    return {};
  }
  std::map<std::string, std::vector<uint8_t>> blobs;
};

TEST(RetiredKeychainBackupTest, IsANoOpWhereThereIsNoStore) {
  EXPECT_TRUE(RemoveRetiredKeychainBackup(NoSecureBlobStore()));
}

TEST(RetiredKeychainBackupTest, DeletesOnlyTheRetiredItem) {
  MemoryStore store;
  ASSERT_TRUE(store.Write("account-backup", {1, 2, 3}));
  ASSERT_TRUE(store.Write("recovery-seed", {4}));
  EXPECT_TRUE(RemoveRetiredKeychainBackup(store));
  EXPECT_EQ(store.blobs.count("account-backup"), 0u);
  EXPECT_EQ(store.blobs.count("recovery-seed"), 1u) << "the keychain store stays for M023's easy tier";
}

} // namespace
} // namespace pbr
