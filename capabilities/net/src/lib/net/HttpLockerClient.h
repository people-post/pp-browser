#pragma once

#include "lib/net/LockerClient.h"
#include "lib/net/OrgBackendClients.h"
#include "lib/net/RelayAudience.h"

#include "common/ExtArea.h"

#include <memory>

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The bytes the relay half of a locker write is signed over (the quota half, by the account; the record's own owner
 * signature names no relay, SIGNED_FORMATS § 2.6): `pp-browser:relay-locker-put` ‖ 0 ‖ u8 4 ‖ LenUtf8 audience (the
 * relay's PeerId, multi-relay MR007) ‖ LenUtf8 Account ID ‖ LenUtf8 locker id ‖ u64 version ‖ i64 timestamp ‖ ext area
 * (§ 2.8). Empty for an empty audience.
 */
std::vector<uint8_t> BuildLockerPutSignBytes(const std::string& audience, const std::string& account_id,
                                             const std::string& locker_id, uint64_t version, int64_t timestamp,
                                             const pp::ExtArea& ext = {});

/** The relay as a locker provider: `POST /v1/lockers/put`, `GET /v1/lockers/:id` (SERVICE_ENDPOINTS). */
class HttpLockerClient : public ILockerClient {
public:
  explicit HttpLockerClient(std::string base_url);

  void SetAuthSigner(RelayAuthSigner signer) { auth_signer_ = std::move(signer); }
  /** Who a write is signed for (the org origin's pin, MR007). */
  void SetAudience(std::shared_ptr<IRelayAudience> audience) { audience_ = std::move(audience); }

  Roe<void> Put(const std::string& account_id, const LockerRecord& record,
                std::optional<LockerPutRefused>* refused = nullptr) override;
  Roe<std::optional<LockerRecord>> Get(const std::string& locker_id) override;

private:
  std::string base_url_;
  RelayAuthSigner auth_signer_;
  std::shared_ptr<IRelayAudience> audience_;
};

} // namespace pbr
