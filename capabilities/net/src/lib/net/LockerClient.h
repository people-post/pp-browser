#pragma once

#include "common/Error.h"
#include "common/Value.h"

#include <cstdint>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * An account locker snapshot as a provider stores it (multi-device M023 §4–5; locker record v2, release-compatibility
 * SIGNED_FORMATS § 2.6): base64 / hex text, opaque to this layer — sealing, reading and verifying are
 * `lib/people/AccountLocker`'s (`account_locker::RecordFromJson`); the feature layer converts.
 */
struct LockerRecord {
  /** The record's format version (`record_version`), as written; 0 when the JSON has none. */
  int64_t record_version = 0;
  /** Lowercase hex of the 32-byte locker id. */
  std::string locker_id;
  /** The revision. */
  uint64_t version = 0;
  /** The typed locker signing key. */
  std::string signing_public_key_b64;
  std::string sealed_b64;
  /** The signed extension entries (`ext_b64`), as written; empty for none. */
  std::string ext_b64;
  /** The typed signature. */
  std::string signature_b64;
};

/** A record as JSON (the relay's `locker` object; also an exported locker file's). */
Object LockerRecordToJson(const LockerRecord& record);
LockerRecord LockerRecordFromJson(const Object& json);

/** Why a locker write did not store. */
enum class LockerPutRefusal {
  /** The provider holds this version or a newer one (LockerPutRefused::stored_version when it said which). */
  NotNewer,
  /** This relay user already holds as many lockers as the provider allows. */
  QuotaReached,
};

/** A locker write the provider refused, and what it said. */
struct LockerPutRefused {
  LockerPutRefusal reason = LockerPutRefusal::NotNewer;
  /** NotNewer: the version the provider holds (the relay's 409 `version`, X12); 0 when it did not say. */
  uint64_t stored_version = 0;
};

/** One locker provider (the relay; later other relays). */
class ILockerClient {
public:
  virtual ~ILockerClient() = default;
  /**
   * Store `record` as `account_id` (the relay checks the request is that user's, and the snapshot is the locker
   * owner's). An error carries a LockerPutRefused in `refused` when the provider said why.
   */
  virtual Roe<void> Put(const std::string& account_id, const LockerRecord& record,
                        std::optional<LockerPutRefused>* refused = nullptr) = 0;
  /** The stored snapshot of `locker_id` (hex), by id alone; nullopt when the provider holds none. */
  virtual Roe<std::optional<LockerRecord>> Get(const std::string& locker_id) = 0;
};

} // namespace pbr
