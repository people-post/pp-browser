#include "lib/people/RetiredKeychainBackup.h"

#include "lib/people/AccountBackupCodec.h"
#include "lib/people/LocalAccountBackup.h"

namespace pbr {

Roe<void> RemoveRetiredKeychainBackup() {
  ISecureBlobStore& keychain = OsSecureBlobStore();
  if (!keychain.Supported()) {
    return {};
  }
  return keychain.Delete(AccountBackupCodec::kBlobName);
}

} // namespace pbr
