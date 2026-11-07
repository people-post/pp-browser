#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "common/Error.h"
#include "common/SecureBlobStore.h"
#include "foundation/crypto/CryptoTypes.h"

namespace pbr {

/**
 * The profile DEK kept outside the PIN vault in two pieces that only open together (at-rest A013 grace window,
 * A014 biometric unlock): a file in the profile directory holding the DEK in an EncryptedPayload envelope under a
 * random 32-byte key, AAD bound to the profile, and a secure-blob entry holding that key. Never the PIN, never the
 * DEK in the clear.
 *
 * The file half and Forget are shared by every user. Seal / Open add the plain entry, version 1 + key 32 bytes
 * (BiometricUnlock); GraceUnlock keeps its own longer entry (key + boot id + expiry) around the file half.
 */
class SealedDek {
public:
  enum class Status {
    Found,       ///< `dek` holds the DEK.
    Missing,     ///< No usable pair (a piece missing, undecryptable, another profile): both pieces were deleted.
    Unavailable, ///< The store cannot be read right now: nothing was deleted, `code` is the OS status.
    Canceled,    ///< The store's system prompt was canceled or not passed: nothing was deleted, `code` as above.
  };
  struct Result {
    Status status = Status::Missing;
    ByteVector dek;
    int64_t code = 0;
  };

  static constexpr size_t kKeySize = 32;

  /** A fresh random key of kKeySize bytes; the caller wipes it. */
  static ByteVector NewKey();

  /** Seals `dek` under `key` into `file` (atomic write). */
  static Roe<void> SealFile(const std::filesystem::path& file, const ByteVector& key, std::string_view aad,
                            const ByteVector& dek);

  /** The DEK from `file`, or nullopt when the file is absent, unreadable, not ours or not a DEK. */
  static std::optional<ByteVector> OpenFile(const std::filesystem::path& file, const ByteVector& key,
                                            std::string_view aad);

  /** A fresh key, `file` sealed, then the entry written under `access`; a failed entry write deletes both. */
  static Roe<void> Seal(ISecureBlobStore& store, const std::string& entry_name, const std::filesystem::path& file,
                        std::string_view aad, const ByteVector& dek, SecureBlobAccess access);

  /**
   * See Status. Without `file` the entry is not read (no system prompt for nothing) and is deleted. A non-empty
   * `prompt` reads through LookupWithPrompt. A store error is returned and deletes nothing.
   */
  static Roe<Result> Open(ISecureBlobStore& store, const std::string& entry_name, const std::filesystem::path& file,
                          std::string_view aad, const std::string& prompt = {});

  /** Deletes both pieces; never fails. */
  static void Forget(ISecureBlobStore& store, const std::string& entry_name, const std::filesystem::path& file);
};

} // namespace pbr
