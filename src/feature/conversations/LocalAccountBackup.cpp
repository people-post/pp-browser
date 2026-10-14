#include "feature/conversations/LocalAccountBackup.h"

#include "foundation/crypto/LinkDeviceCodec.h"
#include "foundation/platform/os/OsSecureBlob.h"
#include "feature/conversations/LinkDeviceCoordinator.h"

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

Roe<std::optional<AccountBackupV1>> ReadBackup(ISecureBlobStore& store) {
  auto bytes = store.Read(AccountBackupCodec::kBlobName);
  if (!bytes) {
    return bytes.error();
  }
  if (!bytes->has_value()) {
    return std::optional<AccountBackupV1>{};
  }
  auto parsed = AccountBackupCodec::Parse(**bytes);
  if (!parsed) {
    return parsed.error();
  }
  return std::optional<AccountBackupV1>(std::move(*parsed));
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
  AccountBackupV1 backup;
  backup.updated_at_ms = now_ms;
  backup.link_bundle_json = std::move(*bundle);
  backup.contacts = std::move(*list);
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
  auto bundle = LinkDeviceCodec::Parse((**backup).link_bundle_json);
  if (!bundle) {
    return bundle.error();
  }
  return std::optional<std::string>(bundle->nickname);
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
  auto imported = LinkDeviceCoordinator::Import(identity, vault, psk_store, secrets, (**backup).link_bundle_json, pin,
                                                now_ms);
  if (!imported) {
    return imported.error();
  }
  for (const Contact& contact : (**backup).contacts) {
    if (auto upserted = contacts.Upsert(contact); !upserted) {
      return upserted.error();
    }
  }
  return {};
}

Roe<void> LocalAccountBackup::Delete(ISecureBlobStore& store) {
  return store.Delete(AccountBackupCodec::kBlobName);
}

} // namespace pbr
