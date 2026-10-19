#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/DataKeyVault.h"
#include "domain/messaging/SqlitePskSessionStore.h"
#include "domain/messaging/SqliteThreadStore.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"
#include "feature/conversations/LocalAccountBackup.h"

#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {
namespace {

class MemoryBlobStore final : public ISecureBlobStore {
public:
  bool supported = true;
  std::map<std::string, std::vector<uint8_t>> blobs;
  bool Supported() const override { return supported; }
  Roe<void> Write(const std::string& name, const std::vector<uint8_t>& bytes) override {
    blobs[name] = bytes;
    return {};
  }
  Roe<std::optional<std::vector<uint8_t>>> Read(const std::string& name) override {
    const auto it = blobs.find(name);
    if (it == blobs.end()) {
      return std::optional<std::vector<uint8_t>>{};
    }
    return std::optional<std::vector<uint8_t>>(it->second);
  }
  Roe<void> Delete(const std::string& name) override {
    blobs.erase(name);
    return {};
  }
};

ByteVector DekOf(const uint8_t fill) {
  return ByteVector(kDataEncryptionKeySize, fill);
}

/** One device's profile: identity, vault, PSK store, contacts in a temp dir. */
struct Device {
  explicit Device(const std::string& tag) {
    dir = std::filesystem::temp_directory_path() / ("pp_local_backup_" + tag + "_" + util::GenerateUuid());
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    threads = std::make_unique<SqliteThreadStore>(dir.string());
    (void)threads->ListThreads();
    psks = std::make_unique<SqlitePskSessionStore>(threads->ProfileDbPath(), tag);
    identity = std::make_unique<IdentityStore>(dir.string(), tag);
    vault = std::make_unique<DataKeyVault>(DataKeyVault::VaultPathForProfile(dir.string()), tag);
    contacts = std::make_unique<ContactsStore>(dir.string());
  }
  ~Device() {
    contacts.reset();
    vault.reset();
    identity.reset();
    psks.reset();
    threads.reset();
    std::filesystem::remove_all(dir);
  }
  std::filesystem::path dir;
  std::unique_ptr<SqliteThreadStore> threads;
  std::unique_ptr<SqlitePskSessionStore> psks;
  std::unique_ptr<IdentityStore> identity;
  std::unique_ptr<DataKeyVault> vault;
  std::unique_ptr<ContactsStore> contacts;
};

constexpr int64_t kNow = 1'700'000'000'000;

void SeedRegisteredAccount(Device& d, const ByteVector& dek) {
  ASSERT_TRUE(d.psks->SetDek(dek));
  ASSERT_TRUE(d.identity->SetDek(dek));
  auto created = d.identity->LoadOrCreate();
  ASSERT_TRUE(created);
  LocalIdentity seeded = *created;
  seeded.relay_user_id = "relay:alice";
  seeded.nickname = "alice";
  seeded.registered = true;
  ASSERT_TRUE(d.identity->Update(seeded));
  ASSERT_TRUE(d.vault->CreateWithDek("old-pin", dek));
  Contact bob;
  bob.id = "contact-bob";
  bob.local.display_name = "Bob from work";
  bob.local.trust = TrustLevel::Friendly;
  bob.remote.nickname = "bob";
  bob.remote.ids = {{mesh::ContactIdKind::Account, "account:bob", true}};
  SyncContactMirrors(bob);
  ASSERT_TRUE(d.contacts->Upsert(bob));
}

TEST(LocalAccountBackupTest, WriteThenRestoreOnAFreshDeviceBringsAccountAndContactsBack) {
  MemoryBlobStore store;
  const ByteVector shared = DekOf(0x31);
  Device old("old");
  SeedRegisteredAccount(old, shared);
  auto old_identity = old.identity->Get();
  ASSERT_TRUE(old_identity);

  auto written = LocalAccountBackup::Write(store, *old.identity, *old.vault, *old.psks, *old.contacts, kNow);
  ASSERT_TRUE(written) << written.error().message;
  ASSERT_EQ(store.blobs.size(), 1u);

  auto nickname = LocalAccountBackup::PeekNickname(store);
  ASSERT_TRUE(nickname) << nickname.error().message;
  ASSERT_TRUE(nickname->has_value());
  EXPECT_EQ(**nickname, "alice");

  // Fresh install, much later: its own vault under a new PIN, nothing else.
  Device fresh("fresh");
  const ByteVector local_dek = DekOf(0x44);
  ASSERT_TRUE(fresh.identity->SetDek(local_dek));
  ASSERT_TRUE(fresh.identity->LoadOrCreate());
  ASSERT_TRUE(fresh.vault->CreateWithDek("new-pin", local_dek));
  ASSERT_TRUE(fresh.psks->SetDek(shared));
  const int64_t years_later = kNow + int64_t{3} * 365 * 24 * 60 * 60 * 1000;

  auto restored = LocalAccountBackup::Restore(store, *fresh.identity, *fresh.vault, *fresh.psks, nullptr,
                                              *fresh.contacts, "new-pin", years_later);
  ASSERT_TRUE(restored) << restored.error().message;

  auto identity = fresh.identity->Get();
  ASSERT_TRUE(identity);
  EXPECT_EQ(identity->account_id, old_identity->account_id);
  EXPECT_EQ(identity->relay_user_id, "relay:alice");
  EXPECT_EQ(identity->nickname, "alice");
  EXPECT_TRUE(identity->registered);

  auto contacts = fresh.contacts->List();
  ASSERT_TRUE(contacts);
  ASSERT_EQ(contacts->size(), 1u);
  EXPECT_EQ((*contacts)[0].id, "contact-bob");
  EXPECT_EQ((*contacts)[0].local.display_name, "Bob from work");
  EXPECT_EQ((*contacts)[0].local.trust, TrustLevel::Friendly);
}

TEST(LocalAccountBackupTest, UnregisteredIdentityWritesNothing) {
  MemoryBlobStore store;
  Device d("unreg");
  const ByteVector dek = DekOf(0x11);
  ASSERT_TRUE(d.psks->SetDek(dek));
  ASSERT_TRUE(d.identity->SetDek(dek));
  ASSERT_TRUE(d.identity->LoadOrCreate());
  ASSERT_TRUE(d.vault->CreateWithDek("pin", dek));

  EXPECT_TRUE(LocalAccountBackup::Write(store, *d.identity, *d.vault, *d.psks, *d.contacts, kNow));
  EXPECT_TRUE(store.blobs.empty());
  auto nickname = LocalAccountBackup::PeekNickname(store);
  ASSERT_TRUE(nickname);
  EXPECT_FALSE(nickname->has_value());
}

TEST(LocalAccountBackupTest, UnsupportedStoreIsANoOpAndRestoreFailsCleanly) {
  MemoryBlobStore store;
  store.supported = false;
  Device d("nostore");
  const ByteVector dek = DekOf(0x22);
  SeedRegisteredAccount(d, dek);

  EXPECT_TRUE(LocalAccountBackup::Write(store, *d.identity, *d.vault, *d.psks, *d.contacts, kNow));
  EXPECT_TRUE(store.blobs.empty());
  EXPECT_TRUE(LocalAccountBackup::Delete(store));
  EXPECT_FALSE(LocalAccountBackup::Restore(store, *d.identity, *d.vault, *d.psks, nullptr, *d.contacts, "pin", kNow));
}

} // namespace
} // namespace pbr
