#pragma once

#include "foundation/crypto/DataKeyVault.h"
#include "foundation/crypto/IPskSessionStore.h"
#include "foundation/crypto/ProfileSecretsEngine.h"
#include "lib/people/AccountBackupCodec.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"
#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

/** Where the backup blob lives. Production: the OS store (iOS Keychain); tests: in memory. */
class ISecureBlobStore {
public:
  virtual ~ISecureBlobStore() = default;
  virtual bool Supported() const = 0;
  virtual Roe<void> Write(const std::string& name, const std::vector<uint8_t>& bytes) = 0;
  virtual Roe<std::optional<std::vector<uint8_t>>> Read(const std::string& name) = 0;
  virtual Roe<void> Delete(const std::string& name) = 0;
};

/** `pbr::os::SecureBlob*` behind the interface. */
ISecureBlobStore& OsSecureBlobStore();

/**
 * Local account backup (DESIGN-account-recovery §9–10, PLAN-local-account-backup): the account as a
 * long-lived link-device bundle plus the address book, kept where an uninstall does not delete it,
 * restored through the ordinary link-device import. Only the nickname is stored in the clear; the
 * rest is sealed under the DEK next to a copy of `vault.bin`, so restoring needs the profile PIN.
 * Every call is synchronous; the hub posts Write onto a worker.
 */
class LocalAccountBackup {
public:
  /** Bundle validity for the stored copy: effectively never expires. */
  static constexpr int64_t kBundleTtlMs = int64_t{100} * 365 * 24 * 60 * 60 * 1000;

  /**
   * Success without writing when the store is unsupported or the identity never registered.
   * Needs the vault unlocked (seals under its DEK). Rewrite after a PIN change.
   */
  static Roe<void> Write(ISecureBlobStore& store, IdentityStore& identity, DataKeyVault& vault,
                         IPskSessionStore& psk_store, ContactsStore& contacts, int64_t now_ms,
                         std::vector<AccountBackupGroup> groups = {});
  /** Nickname of the stored account; nullopt when nothing usable is stored. */
  static Roe<std::optional<std::string>> PeekNickname(ISecureBlobStore& store);
  /**
   * Fresh install: `pin` must be the PIN the account used (it opens the stored vault copy; wrong
   * PIN → Pin-category error, nothing written). Then imports the bundle under that same PIN (vault +
   * account + public PSKs, like a pasted transfer code) and puts the contacts back. Fails when
   * nothing is stored. The groups it kept go to `restored_groups`, for the caller to recreate once
   * messaging is up (each then checks its members, G013).
   */
  static Roe<void> Restore(ISecureBlobStore& store, IdentityStore& identity, DataKeyVault& vault,
                           IPskSessionStore& psk_store, ProfileSecretsEngine* secrets, ContactsStore& contacts,
                           std::string_view pin, int64_t now_ms,
                           std::vector<AccountBackupGroup>* restored_groups = nullptr);
  static Roe<void> Delete(ISecureBlobStore& store);
};

} // namespace pbr
