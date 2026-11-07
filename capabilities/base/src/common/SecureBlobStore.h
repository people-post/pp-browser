#pragma once

#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

/**
 * Small named blobs kept where an app uninstall does not delete them (iOS: the Keychain). Used today by the
 * retired-item cleanup (lib/people RetiredKeychainBackup, at-rest A011) and later by the recovery seed's easy
 * tier (multi-device M023 §3). The app hands in the OS store (foundation/platform OsSecureBlobStore); tests,
 * one in memory.
 */
/** When the OS lets the blob be read (iOS Keychain accessibility; other stores ignore it). */
enum class SecureBlobAccess {
  kAfterFirstUnlock, ///< After the device unlocked once since boot (the default of every blob before 2026-11-06).
  kWhenUnlocked,     ///< Only while the device is unlocked (the PIN grace key, #21 phase 1).
  /// Only after the enrolled face / finger passes the system prompt, and gone for good once the enrolment changes
  /// (iOS kSecAccessControlBiometryCurrentSet, this device only; the biometric unlock key, at-rest A014). Stores
  /// that cannot hold it answer Supports() false and refuse the write.
  kBiometryCurrentSet,
};

/** A read that tells "nothing stored" from "stored but not readable right now" (iOS: device locked). */
struct SecureBlobLookup {
  enum class Status {
    Found,
    Missing,
    Unavailable,
    Canceled, ///< The system prompt of a kBiometryCurrentSet item was canceled or not passed; the item stays.
  };
  Status status = Status::Missing;
  std::vector<uint8_t> bytes; ///< Only with Found.
  int64_t code = 0;           ///< The OS status with Unavailable and Canceled (0 when the store has none).
};

class ISecureBlobStore {
public:
  virtual ~ISecureBlobStore() = default;
  virtual bool Supported() const = 0;
  virtual Roe<void> Write(const std::string& name, const std::vector<uint8_t>& bytes) = 0;
  virtual Roe<std::optional<std::vector<uint8_t>>> Read(const std::string& name) = 0;
  virtual Roe<void> Delete(const std::string& name) = 0;
  /**
   * Write under an access level. Stores without levels (and the default here) ignore it, except that the default
   * refuses kBiometryCurrentSet: a blob meant to need the face / finger is never kept without it.
   */
  virtual Roe<void> Write(const std::string& name, const std::vector<uint8_t>& bytes, SecureBlobAccess access) {
    if (access == SecureBlobAccess::kBiometryCurrentSet) {
      return Error("Secure blob store cannot hold a biometry-bound blob");
    }
    return Write(name, bytes);
  }
  /** Whether this store can hold a blob under `access`. The default: every level but kBiometryCurrentSet. */
  virtual bool Supports(SecureBlobAccess access) const {
    return Supported() && access != SecureBlobAccess::kBiometryCurrentSet;
  }
  /** Read with the three-state answer. The default maps Read(): error → Unavailable, nullopt → Missing. */
  virtual Roe<SecureBlobLookup> Lookup(const std::string& name) {
    auto got = Read(name);
    SecureBlobLookup out;
    if (!got) {
      out.status = SecureBlobLookup::Status::Unavailable;
      return out;
    }
    if (!got->has_value()) {
      return out;
    }
    out.status = SecureBlobLookup::Status::Found;
    out.bytes = std::move(**got);
    return out;
  }
  /**
   * Lookup() for an item whose read shows a system prompt (kBiometryCurrentSet): `prompt` is the line the prompt
   * shows. Blocks until the user answers, so never on the UI thread. Stores without prompts: Lookup().
   */
  virtual Roe<SecureBlobLookup> LookupWithPrompt(const std::string& name, const std::string& /*prompt*/) {
    return Lookup(name);
  }
};

/** No store: unsupported, writes nothing, reads nothing. */
inline ISecureBlobStore& NoSecureBlobStore() {
  class NoStore final : public ISecureBlobStore {
  public:
    bool Supported() const override { return false; }
    Roe<void> Write(const std::string&, const std::vector<uint8_t>&) override { return {}; }
    Roe<std::optional<std::vector<uint8_t>>> Read(const std::string&) override {
      return std::optional<std::vector<uint8_t>>{};
    }
    Roe<void> Delete(const std::string&) override { return {}; }
  };
  static NoStore store;
  return store;
}

} // namespace pbr
