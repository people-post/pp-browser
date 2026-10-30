#pragma once

#include "common/Error.h"
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The app version from which RemoveRetiredKeychainBackup is itself removed (at-rest A011): by then every
 * phone that had the item has launched a build that deleted it. retired_keychain_backup_test fails from this
 * version on, so the release bump that reaches it removes the cleanup. The iOS Keychain store itself stays: the
 * recovery seed's easy tier keeps it in the synced keychain (multi-device M023 §3).
 */
inline constexpr const char kRetiredKeychainBackupRemoveAt[] = "1.1.0";

/**
 * at-rest A011: delete the account backup item earlier builds left in the iOS Keychain. Restore is unsupported,
 * so nothing reads it, and it held the account's secret keys under the PIN past an uninstall. Idempotent; a
 * no-op where the OS has no such store.
 */
Roe<void> RemoveRetiredKeychainBackup();

} // namespace pbr
