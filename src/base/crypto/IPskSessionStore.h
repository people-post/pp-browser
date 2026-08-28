#pragma once

#include "base/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

struct RetiredPskEntry {
  uint32_t epoch = 0;
  std::string master_psk_b64;
  int64_t retired_at = 0;
};

struct PskBundleV1 {
  CryptoChannel channel = CryptoChannel::E2e;
  uint32_t active_epoch = 1;
  std::string master_psk_b64;
  std::vector<RetiredPskEntry> retired_epochs;
};

struct PskSessionRecord {
  ChatTargetKey key;
  uint32_t session_epoch = 1;
  std::optional<std::string> master_psk_b64;
  std::optional<std::string> psk_fingerprint;
  std::optional<int64_t> psk_verified_at;
  std::vector<RetiredPskEntry> retired_psks;
  PublicKeyScope key_scope = PublicKeyScope::Account;
  std::optional<std::string> thread_kem_pk_b64;
  std::optional<std::string> thread_kem_sk_b64;
  std::optional<std::string> peer_thread_kem_pk_b64;
  std::optional<int64_t> last_psk_rotate_at;
  uint32_t psk_rotate_msg_count = 0;
  std::optional<std::string> last_rotation_id;
};

class IPskSessionStore {
public:
  virtual ~IPskSessionStore() = default;

  virtual Roe<std::optional<PskSessionRecord>> Load(const ChatTargetKey& key) const = 0;
  /** Rows with a master PSK or retired tail (plaintext after DEK decrypt). */
  virtual Roe<std::vector<PskSessionRecord>> List() const = 0;
  virtual Roe<void> Save(const PskSessionRecord& record) = 0;
  virtual Roe<ByteVector> GenerateMasterPsk() = 0;
  virtual Roe<std::optional<std::string>> ResolveMasterPskForEpoch(const ChatTargetKey& key,
                                                                 uint32_t envelope_epoch) const = 0;
  virtual Roe<void> MarkPskVerified(const ChatTargetKey& key, int64_t verified_at_ms) = 0;
  virtual Roe<bool> IsPskVerified(const ChatTargetKey& key) const = 0;
  virtual Roe<PskBundleV1> ExportPskBundle(const ChatTargetKey& key) const = 0;
  virtual Roe<void> ImportPskBundle(const ChatTargetKey& key, const PskBundleV1& bundle) = 0;
};

} // namespace pbr
