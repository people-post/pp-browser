#include "lib/people/RetiredKeychainBackup.h"

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
