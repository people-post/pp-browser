#pragma once

#include "lib/net/LockerClient.h"
#include "lib/net/OrgBackendClients.h"

#include <cstdint>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The bytes the relay half of a locker write is signed over (www `buildLockerPutSignBytes`):
 * `pp-browser:relay-locker-put-v1` ‖ 0 ‖ 1 ‖ LenUtf8 relay user ‖ LenUtf8 locker id ‖ u64 version ‖ i64 timestamp.
 */
std::vector<uint8_t> BuildLockerPutSignBytes(const std::string& relay_user_id, const std::string& locker_id,
                                             uint64_t version, int64_t timestamp);

/** The relay as a locker provider: `POST /v1/lockers/put`, `GET /v1/lockers/:id` (SERVICE_ENDPOINTS). */
class HttpLockerClient : public ILockerClient {
public:
  explicit HttpLockerClient(std::string base_url);

  void SetAuthSigner(RelayAuthSigner signer) { auth_signer_ = std::move(signer); }

  Roe<void> Put(const std::string& relay_user_id, const LockerRecord& record,
                std::optional<LockerPutRefusal>* refused = nullptr) override;
  Roe<std::optional<LockerRecord>> Get(const std::string& locker_id) override;

private:
  std::string base_url_;
  RelayAuthSigner auth_signer_;
};

} // namespace pbr
