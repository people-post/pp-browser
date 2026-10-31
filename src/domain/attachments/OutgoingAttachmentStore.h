#pragma once

#include "common/chat/ChatPayloadTypes.h"
#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** What is known about an attachment on its way out: enough to send it again without the original file. */
struct OutgoingAttachmentRecord {
  std::string id;
  std::string thread_id;
  /** Everything the message will carry; `url` stays empty until the ciphertext is on the server. */
  ChatAttachmentFields fields;
  uint64_t cipher_bytes = 0;
  /** The ciphertext has reached the recipient (server copy or direct push): only the message is left to send. */
  bool delivered = false;
  int64_t created_ms = 0;
};

/**
 * Ciphertext waiting to be (re)sent: `{profile}/blob_cipher/outgoing/<id>` is the AEAD ciphertext, `<id>.rec` the
 * record (including the content key) sealed under the profile DEK. The ciphertext alone reveals nothing; the key
 * is only in the sealed record. Removed when the message is sent, when the person discards it, and at start
 * when older than a day.
 */
class OutgoingAttachmentStore {
public:
  OutgoingAttachmentStore(std::string profile_dir, std::string profile_id);

  static constexpr int64_t kMaxAgeMs = 24LL * 60LL * 60LL * 1000LL;

  std::string Dir() const;
  std::string CipherPath(const std::string& id) const;

  /** A new record id (random, filesystem-safe). */
  static std::string NewId();

  /** Write the ciphertext, then the sealed record. */
  Roe<void> Begin(const OutgoingAttachmentRecord& record, const std::vector<uint8_t>& ciphertext, const ByteVector& dek);
  Roe<void> Update(const OutgoingAttachmentRecord& record, const ByteVector& dek);
  Roe<OutgoingAttachmentRecord> Load(const std::string& id, const ByteVector& dek) const;
  /** The records of a thread whose ciphertext file is still there, oldest first; unreadable ones are skipped. */
  std::vector<OutgoingAttachmentRecord> ListForThread(const std::string& thread_id, const ByteVector& dek) const;
  bool HasCipher(const std::string& id) const;
  Roe<std::vector<uint8_t>> ReadCipher(const std::string& id) const;
  void Remove(const std::string& id) const;
  /** Remove every file older than `max_age_ms` (and strays without a pair); returns how many ids went. */
  size_t CleanupStale(int64_t max_age_ms) const;

private:
  std::string RecordPath(const std::string& id) const;

  std::string profile_dir_;
  std::string profile_id_;
};

} // namespace pbr
