#include "lib/people/RetiredKeychainBackup.h"

#include "lib/people/SecureBlobStore.h"

namespace pbr {

namespace {
/** The Keychain item name earlier builds wrote the account backup under; goes with the cleanup at 1.1.0 (at-rest A011). */
constexpr const char kRetiredBackupItemName[] = "account-backup";
} // namespace

Roe<void> RemoveRetiredKeychainBackup() {
  ISecureBlobStore& keychain = OsSecureBlobStore();
  if (!keychain.Supported()) {
    return {};
  }
  return keychain.Delete(kRetiredBackupItemName);
}

} // namespace pbr
