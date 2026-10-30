#pragma once

#include "common/Error.h"
#include "common/Value.h"

#include <cstdint>
#include <optional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * An account locker snapshot as a provider stores it (multi-device M023 §4–5): base64 / hex text, opaque to this
 * layer — sealing and verifying are `lib/people/AccountLocker`'s; the feature layer converts.
 */
struct LockerRecord {
  /** Lowercase hex of the 32-byte locker id. */
  std::string locker_id;
  uint64_t version = 0;
  std::string signing_public_key_b64;
  std::string sealed_b64;
  std::string signature_b64;
};

/** A record as JSON (the relay's `locker` object; also an exported locker file's). */
Object LockerRecordToJson(const LockerRecord& record);
LockerRecord LockerRecordFromJson(const Object& json);

/** Why a locker write did not store. */
enum class LockerPutRefusal {
  /** The provider holds this version or a newer one (`stored_version` when it said which). */
  NotNewer,
  /** This relay user already holds as many lockers as the provider allows. */
  QuotaReached,
};

/** One locker provider (the relay; later other relays). */
class ILockerClient {
public:
  virtual ~ILockerClient() = default;
  /**
   * Store `record` as `relay_user_id` (the relay checks the request is that user's, and the snapshot is the locker
   * owner's). An error carries a LockerPutRefusal in `refused` when the provider said why.
   */
  virtual Roe<void> Put(const std::string& relay_user_id, const LockerRecord& record,
                        std::optional<LockerPutRefusal>* refused = nullptr) = 0;
  /** The stored snapshot of `locker_id` (hex), by id alone; nullopt when the provider holds none. */
  virtual Roe<std::optional<LockerRecord>> Get(const std::string& locker_id) = 0;
};

} // namespace pbr
