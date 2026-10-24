#include "lib/people/LocalAccountBackup.h"

#include "foundation/crypto/PskSessionRestart.h"

#include "foundation/crypto/FileCipher.h"
#include "foundation/platform/os/OsSecureBlob.h"
#include "lib/people/LinkDeviceCoordinator.h"

#include <string>
#include <vector>

namespace pbr {

namespace {

class OsStore final : public ISecureBlobStore {
public:
  bool Supported() const override { return os::SecureBlobSupported(); }
  Roe<void> Write(const std::string& name, const std::vector<uint8_t>& bytes) override {
    return os::SecureBlobWrite(name, bytes);
  }
  Roe<std::optional<std::vector<uint8_t>>> Read(const std::string& name) override { return os::SecureBlobRead(name); }
  Roe<void> Delete(const std::string& name) override { return os::SecureBlobDelete(name); }
};

Roe<std::optional<AccountBackupV2>> ReadBackup(ISecureBlobStore& store) {
  auto bytes = store.Read(AccountBackupCodec::kBlobName);
  if (!bytes) {
    return bytes.error();
  }
  if (!bytes->has_value()) {
    return std::optional<AccountBackupV2>{};
  }
  auto parsed = AccountBackupCodec::Parse(**bytes);
  if (!parsed) {
    return parsed.error();
  }
  return std::optional<AccountBackupV2>(std::move(*parsed));
}

/** The sealed contents are bound to the profile id of the vault stored next to them. */
std::string SealedAad(const std::string& profile_id) {
  return FileCipher::BuildAad(AccountBackupCodec::kBlobName, profile_id, AccountBackupCodec::kSchemaVersion);
}

} // namespace

ISecureBlobStore& OsSecureBlobStore() {
  static OsStore store;
  return store;
}

Roe<void> LocalAccountBackup::Write(ISecureBlobStore& store, IdentityStore& identity, DataKeyVault& vault,
                                    IPskSessionStore& psk_store, ContactsStore& contacts, const int64_t now_ms) {
  if (!store.Supported()) {
    return {};
  }
  auto loaded = identity.Get();
  if (!loaded) {
    return loaded.error();
  }
  if (!loaded->registered || loaded->relay_user_id.empty()) {
    return {}; // nothing worth bringing back yet; the bundle needs the relay binding anyway
  }
  auto bundle = LinkDeviceCoordinator::ExportJson(identity, vault, psk_store, now_ms, kBundleTtlMs);
  if (!bundle) {
    return bundle.error();
  }
  auto list = contacts.List();
  if (!list) {
    return list.error();
  }
  auto dek = vault.Dek();
  if (!dek) {
    return dek.error();
  }
  auto vault_bytes = vault.ReadVaultFile();
  if (!vault_bytes) {
    return vault_bytes.error();
  }
  auto contents = AccountBackupCodec::SerializeContents({std::move(*bundle), std::move(*list)});
  if (!contents) {
    return contents.error();
  }
  auto sealed = FileCipher::Encrypt(*dek, *contents, SealedAad(vault.ProfileId()));
  if (!sealed) {
    return sealed.error();
  }
  AccountBackupV2 backup;
  backup.updated_at_ms = now_ms;
  backup.nickname = loaded->nickname;
  backup.profile_id = vault.ProfileId();
  backup.vault = std::move(*vault_bytes);
  backup.sealed = std::move(*sealed);
  auto bytes = AccountBackupCodec::Serialize(backup);
  if (!bytes) {
    return bytes.error();
  }
  return store.Write(AccountBackupCodec::kBlobName, *bytes);
}

Roe<std::optional<std::string>> LocalAccountBackup::PeekNickname(ISecureBlobStore& store) {
  auto backup = ReadBackup(store);
  if (!backup) {
    return backup.error();
  }
  if (!backup->has_value()) {
    return std::optional<std::string>{};
  }
  return std::optional<std::string>((**backup).nickname);
}

Roe<void> LocalAccountBackup::Restore(ISecureBlobStore& store, IdentityStore& identity, DataKeyVault& vault,
                                      IPskSessionStore& psk_store, ProfileSecretsEngine* secrets,
                                      ContactsStore& contacts, const std::string_view pin, const int64_t now_ms) {
  auto backup = ReadBackup(store);
  if (!backup) {
    return backup.error();
  }
  if (!backup->has_value()) {
    return Error("No local account backup");
  }
  // The PIN the account used opens the stored vault copy; a wrong one fails here, before anything is written.
  auto dek = DataKeyVault::UnwrapDekFromVaultBytes((**backup).vault, (**backup).profile_id, pin);
  if (!dek) {
    return dek.error();
  }
  auto opened = FileCipher::Decrypt(*dek, (**backup).sealed, SealedAad((**backup).profile_id));
  if (!opened) {
    return Error("Account backup: contents do not open");
  }
  auto contents = AccountBackupCodec::ParseContents(*opened);
  if (!contents) {
    return contents.error();
  }
  // From here on the PIN is known to be right: report later failures outside the PIN category so
  // the gate does not show them as a wrong PIN.
  auto imported =
      LinkDeviceCoordinator::Import(identity, vault, psk_store, secrets, contents->link_bundle_json, pin, now_ms);
  if (!imported) {
    return Error("Account restore failed: " + imported.error().message);
  }
  // The sessions are back but this device's message numbering is not: each pair starts its next epoch
  // with the next send (D014), so the peer never sees a seq it already holds (#378).
  if (auto marked = MarkPublicSessionsForRestart(psk_store); !marked) {
    return Error("Account restore failed: " + marked.error().message);
  }
  for (const Contact& contact : contents->contacts) {
    if (auto upserted = contacts.Upsert(contact); !upserted) {
      return Error("Account restore failed: " + upserted.error().message);
    }
  }
  return {};
}

Roe<void> LocalAccountBackup::Delete(ISecureBlobStore& store) {
  return store.Delete(AccountBackupCodec::kBlobName);
}

} // namespace pbr
