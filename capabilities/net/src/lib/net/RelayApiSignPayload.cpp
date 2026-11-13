#include "lib/net/RelayApiSignPayload.h"

#include "lib/net/RelaySignBytes.h"

namespace pbr {

namespace {

constexpr const char kRelayApiDomain[] = "pp-browser:relay-api-v1";
// 2 (multi-relay MR001 / MR003): every id in these calls is an Account ID; 1 named the relay's own `relay:` ids.
constexpr uint8_t kSignVersion = 2;

} // namespace

void AppendRelayApiHeader(std::ostringstream& oss, RelayApiOp op, const int64_t timestamp) {
  RelaySignAppendDomain(oss, kRelayApiDomain);
  RelaySignAppendU8(oss, kSignVersion);
  RelaySignAppendU8(oss, static_cast<uint8_t>(op));
  RelaySignAppendI64(oss, timestamp);
}

std::vector<uint8_t> BuildRelayApiDeviceSignBytes(const RelayApiOp op, const std::string& account_id,
                                                  const std::string& platform, const std::string& device_id,
                                                  const std::string& push_token, const int64_t timestamp) {
  std::ostringstream oss;
  AppendRelayApiHeader(oss, op, timestamp);
  RelaySignAppendWireLenUtf8(oss, account_id);
  RelaySignAppendWireLenUtf8(oss, platform);
  RelaySignAppendWireLenUtf8(oss, device_id);
  RelaySignAppendWireLenUtf8(oss, push_token);
  return RelaySignOssToBytes(oss);
}

} // namespace pbr
