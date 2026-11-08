#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "common/Error.h"
#include "common/SecureBlobStore.h"
#include "foundation/crypto/CryptoTypes.h"

namespace pbr {

/**
 * Face / fingerprint unlock (at-rest A014, DEV/PLAN-pin-gate-redesign.md C): the DEK in two pieces like the grace
 * window, without a clock. `<profile>/biometric.bin` (an EncryptedPayload envelope over the DEK, keyed by K_bio,
 * AAD "biometric|<profile_id>") and the secure-blob entry `<profile_id>/unlock_biometric` (version 1 + K_bio 32
 * bytes) under SecureBlobAccess::kBiometryCurrentSet: reading it shows the system face / finger prompt, and a change
 * of the enrolled faces / fingers makes it unreadable for good (→ Missing). Never the PIN, never the DEK in the clear.
 *
 * It does not check the DEK against the vault (nothing on disk can, without the PIN): ProfileSecretsEngine does
 * what it can, and every path that replaces the DEK must Disable().
 */
class BiometricUnlock {
public:
  enum class Status {
    Found,       ///< `dek` holds the DEK.
    Missing,     ///< Not enabled, the entry is gone (enrolment changed), or the pair does not open: both deleted.
    Unavailable, ///< The prompt could not run now (app sent to the background, a call): nothing deleted.
    Canceled,    ///< The user canceled, or the face / finger did not pass: nothing deleted.
  };
  struct Result {
    Status status = Status::Missing;
    ByteVector dek;
    int64_t code = 0; ///< The OS status with Unavailable / Canceled (iOS: -25293 = not passed, -128 = canceled).
  };

  BiometricUnlock(ISecureBlobStore& store, std::filesystem::path profile_dir, std::string profile_id);

  /** Whether the store can hold a kBiometryCurrentSet entry here. */
  bool Supported() const;
  /** `biometric.bin` exists (the switch's state; the entry itself cannot be checked without a prompt). */
  bool Enabled() const;

  /**
   * See Status. Shows the system prompt with `prompt` and blocks until it is answered: never on the UI thread.
   * Without `biometric.bin` (or where not Supported) it answers Missing at once, with no prompt. A store error is
   * returned and deletes nothing.
   */
  Roe<Result> TryUnlock(const std::string& prompt);

  /** A fresh K_bio, both pieces written (an older entry under the name is replaced). */
  Roe<void> Enable(const ByteVector& dek);

  /** Deletes both pieces; never fails, never prompts. */
  void Disable();

  /** The store entry's name for this profile. */
  std::string EntryName() const;
  /** The on-disk piece's path. */
  std::filesystem::path FilePath() const;

private:
  ISecureBlobStore& store_;
  std::filesystem::path profile_dir_;
  std::string profile_id_;
};

} // namespace pbr
