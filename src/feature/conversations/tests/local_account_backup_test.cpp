#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/DataKeyVault.h"
#include "foundation/crypto/PinDefaults.h"
#include "foundation/error/AppError.h"
#include "domain/messaging/SqlitePskSessionStore.h"
#include "domain/messaging/SqliteThreadStore.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"
#include "domain/people/LocalAccountBackup.h"

#include "common/Utilities.h"
#include "common/ValueJson.h"

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

void SeedRegisteredAccount(Device& d, const ByteVector& dek, const std::string& pin = "old-pin") {
  ASSERT_TRUE(d.psks->SetDek(dek));
  ASSERT_TRUE(d.identity->SetDek(dek));
  auto created = d.identity->LoadOrCreate();
  ASSERT_TRUE(created);
  LocalIdentity seeded = *created;
  seeded.relay_user_id = "relay:alice";
  seeded.nickname = "alice";
  seeded.registered = true;
  ASSERT_TRUE(d.identity->Update(seeded));
  ASSERT_TRUE(d.vault->CreateWithDek(pin, dek));
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

  // Fresh install, much later: no vault yet; the user enters the PIN they used before.
  Device fresh("fresh");
  ASSERT_TRUE(fresh.psks->SetDek(shared));
  const int64_t years_later = kNow + int64_t{3} * 365 * 24 * 60 * 60 * 1000;

  auto restored = LocalAccountBackup::Restore(store, *fresh.identity, *fresh.vault, *fresh.psks, nullptr,
                                              *fresh.contacts, "old-pin", years_later);
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

/** Every file under `dir` with its size: "nothing was written" means this does not change. */
std::map<std::string, uintmax_t> FilesUnder(const std::filesystem::path& dir) {
  std::map<std::string, uintmax_t> files;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
    if (entry.is_regular_file()) {
      files[entry.path().string()] = entry.file_size();
    }
  }
  return files;
}

// The backup outlives an uninstall, so holding the phone must not be enough: restoring needs the PIN
// the account used. A wrong PIN fails with a PIN error and writes nothing; the right one brings the
// same account, keys, data key and contacts back, and the new vault opens with that same PIN.
TEST(LocalAccountBackupTest, RestoreNeedsOriginalPin) {
  MemoryBlobStore store;
  const ByteVector dek = DekOf(0x52);
  Device a("pin_a");
  SeedRegisteredAccount(a, dek, "9876");
  auto a_identity = a.identity->Get();
  ASSERT_TRUE(a_identity);
  auto written = LocalAccountBackup::Write(store, *a.identity, *a.vault, *a.psks, *a.contacts, kNow);
  ASSERT_TRUE(written) << written.error().message;

  Device b("pin_b");
  ASSERT_TRUE(b.psks->SetDek(dek));
  const auto before = FilesUnder(b.dir);

  auto wrong = LocalAccountBackup::Restore(store, *b.identity, *b.vault, *b.psks, nullptr, *b.contacts, "0000", kNow);
  ASSERT_FALSE(wrong) << "restore under a PIN the account never used must fail";
  EXPECT_EQ(AppError::CategoryOf(wrong.error()), ErrorCategory::Pin);
  EXPECT_EQ(FilesUnder(b.dir), before);
  EXPECT_FALSE(b.vault->Exists());
  auto none = b.contacts->List();
  ASSERT_TRUE(none);
  EXPECT_TRUE(none->empty());

  auto right = LocalAccountBackup::Restore(store, *b.identity, *b.vault, *b.psks, nullptr, *b.contacts, "9876", kNow);
  ASSERT_TRUE(right) << right.error().message;
  auto b_identity = b.identity->Get();
  ASSERT_TRUE(b_identity);
  EXPECT_EQ(b_identity->account_id, a_identity->account_id);
  EXPECT_EQ(b_identity->account_signing_private_key_b64, a_identity->account_signing_private_key_b64);
  auto b_dek = b.vault->Dek();
  ASSERT_TRUE(b_dek);
  EXPECT_EQ(*b_dek, dek);
  auto contacts = b.contacts->List();
  ASSERT_TRUE(contacts);
  ASSERT_EQ(contacts->size(), 1u);
  EXPECT_EQ((*contacts)[0].id, "contact-bob");

  b.vault->Lock();
  EXPECT_FALSE(b.vault->Unlock("0000"));
  EXPECT_TRUE(b.vault->Unlock("9876"));
}

// An account that kept the app default PIN restores with it (the restore page tries it first).
TEST(LocalAccountBackupTest, DefaultPinAccountRestoresWithTheDefaultPin) {
  MemoryBlobStore store;
  const ByteVector dek = DekOf(0x53);
  Device a("default_a");
  SeedRegisteredAccount(a, dek, std::string(kDefaultProfilePin));
  ASSERT_TRUE(LocalAccountBackup::Write(store, *a.identity, *a.vault, *a.psks, *a.contacts, kNow));

  Device b("default_b");
  ASSERT_TRUE(b.psks->SetDek(dek));
  auto restored = LocalAccountBackup::Restore(store, *b.identity, *b.vault, *b.psks, nullptr, *b.contacts,
                                              kDefaultProfilePin, kNow);
  ASSERT_TRUE(restored) << restored.error().message;
  auto identity = b.identity->Get();
  ASSERT_TRUE(identity);
  EXPECT_EQ(identity->relay_user_id, "relay:alice");
}

// The stored bytes carry only the nickname and opaque blobs: no data key, no account secret key,
// no contact in the clear.
TEST(LocalAccountBackupTest, StoredBytesHoldNoSecretsInTheClear) {
  MemoryBlobStore store;
  const ByteVector dek = DekOf(0x54);
  Device a("clear_a");
  SeedRegisteredAccount(a, dek, "9876");
  auto identity = a.identity->Get();
  ASSERT_TRUE(identity);
  ASSERT_TRUE(LocalAccountBackup::Write(store, *a.identity, *a.vault, *a.psks, *a.contacts, kNow));
  ASSERT_EQ(store.blobs.size(), 1u);
  const std::vector<uint8_t>& bytes = store.blobs.begin()->second;
  const std::string text(bytes.begin(), bytes.end());

  auto root = ParseObject(text);
  ASSERT_TRUE(root);
  EXPECT_EQ(root->size(), 7u);
  for (const char* key : {"schema", "version", "updated_at_ms", "nickname", "profile_id", "vault_b64", "sealed_b64"}) {
    EXPECT_TRUE(root->contains(key)) << key;
  }
  EXPECT_EQ(ObjectString(*root, "nickname").value_or(""), "alice");
  EXPECT_EQ(text.find(Base64Encode(dek)), std::string::npos);
  ASSERT_FALSE(identity->account_signing_private_key_b64.empty());
  EXPECT_EQ(text.find(identity->account_signing_private_key_b64), std::string::npos);
  ASSERT_FALSE(identity->kem_private_key_b64.empty());
  EXPECT_EQ(text.find(identity->kem_private_key_b64), std::string::npos);
  EXPECT_EQ(text.find("contact-bob"), std::string::npos);
  EXPECT_EQ(text.find("Bob from work"), std::string::npos);
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
