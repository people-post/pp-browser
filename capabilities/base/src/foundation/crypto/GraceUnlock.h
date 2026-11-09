#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

#include "common/Error.h"
#include "common/SecureBlobStore.h"
#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/CryptoTypes.h"

namespace pbr {

/**
 * A moment on the device's own clocks, never the wall clock (DEV/PLAN-pin-grace.md, #21 phase 1): `boot_id` names
 * this boot session (iOS kern.bootsessionuuid, Android BOOT_COUNT; all zero where the platform has none), and
 * `mono_seconds` is a clock that only moves forward within it (iOS mach_continuous_time, Android elapsedRealtime).
 * A grace entry from another boot is expired; changing the system time changes nothing.
 *
 * `mono_seconds <= 0` means "no clock" (the platform could not read it): GraceUnlock fails closed. TryUnlock answers
 * Missing without reading or deleting anything; Remember and Touch succeed and write nothing (0 minutes still
 * forgets, as always).
 */
struct GraceClock {
  std::array<uint8_t, 16> boot_id{};
  int64_t mono_seconds = 0;
};

/**
 * The PIN grace window: after a PIN unlock the DEK is kept for a while so the next cold start (or an incoming call)
 * does not ask again. Two pieces, both deleted together: `<profile>/grace.bin` (an EncryptedPayload envelope over
 * the DEK, keyed by K_grace, AAD "grace|<profile_id>") and the secure-blob entry `<profile_id>/unlock_grace`
 * (version 1 + K_grace 32 bytes + boot_id 16 + expires_mono int64 LE). The store's entry is the only commit point:
 * Touch rewrites it alone. Never the PIN, never the DEK in the clear.
 *
 * Where the store is not Supported() every call returns at once and nothing is read or written.
 */
class GraceUnlock {
public:
  enum class Status {
    Found,       ///< `dek` holds the DEK; the window is still open.
    Expired,     ///< Past its end, or from another boot: both pieces were deleted.
    Missing,     ///< No usable entry (one piece missing, undecryptable, another profile): both pieces were deleted.
    Unavailable, ///< The store cannot be read right now (device locked): nothing was deleted, try again later.
  };
  struct Result {
    Status status = Status::Missing;
    ByteVector dek;
  };

  GraceUnlock(ISecureBlobStore& store, std::filesystem::path profile_dir, std::string profile_id);

  bool Supported() const;

  /** See Status. Only Missing and Expired delete; any other failure keeps both pieces and is reported. */
  Roe<Result> TryUnlock(const GraceClock& now);

  /**
   * A fresh K_grace, both pieces written; `minutes` 0 forgets instead. `kUnlockGraceAlways` writes an expiry of
   * INT64_MAX (open for the rest of this boot; the entry format is unchanged). Only this creates an entry. */
  Roe<void> Remember(const ByteVector& dek, int minutes, const GraceClock& now);

  /**
   * Moves the end of the window to now + minutes by rewriting the store entry alone. Without an entry it does
   * nothing (a Forget stays forgotten); Unavailable is skipped, not an error.
   */
  Roe<void> Touch(int minutes, const GraceClock& now);

  /** Deletes both pieces; never fails. */
  void Forget();

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
