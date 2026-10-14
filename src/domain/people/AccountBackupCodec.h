#pragma once

#include "domain/people/ContactTypes.h"
#include "common/Error.h"

#include <cstdint>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

/**
 * Local account backup (DESIGN-account-recovery §9–10): the account as a never-expiring
 * `pp-browser-link-device-v1` bundle plus the address book, kept where an uninstall does not
 * delete it (iOS Keychain) and restored through the ordinary link-device import. Later the same
 * blob moves onto the ledger / brief backend.
 */
struct AccountBackupV1 {
  int64_t updated_at_ms = 0;
  /** Serialized `LinkDeviceBundleV1` (account keys, DEK, relay id, nickname, public PSKs). */
  std::string link_bundle_json;
  std::vector<Contact> contacts;
};

class AccountBackupCodec {
public:
  static constexpr int kSchemaVersion = 1;
  /** Name of the blob in the secure store. */
  static constexpr const char kBlobName[] = "account-backup";

  static Roe<std::vector<uint8_t>> Serialize(const AccountBackupV1& backup);
  static Roe<AccountBackupV1> Parse(const std::vector<uint8_t>& bytes);
};

} // namespace pbr
